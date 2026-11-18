#include "engine/TrackDecoder.h"

#include "dsp/SincResampler.h"
#include "engine/Mp3Gapless.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <future>
#include <span>
#include <vector>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h first
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>
#endif

namespace devildj::engine
{

namespace
{
using DecodeResult = util::Result<std::unique_ptr<TrackBuffer>>;

constexpr int kChunkSamples = 1 << 16;

// Decoded audio before stereo mapping / resampling: one vector per source channel (at most 2 kept).
struct RawAudio
{
    std::vector<std::vector<float>> channels;
    double containerSeconds = 0.0; // duration the file itself declares (Media Foundation), 0 = unknown
    double sampleRate = 0.0;
    int sourceChannels = 0;
    juce::String formatName;
};

bool cancelled(const DecodeOptions& o) noexcept
{
    return o.cancel != nullptr && o.cancel->load(std::memory_order_relaxed);
}

// Decoding is reported as 0..kDecodeShare of the progress, resampling as the rest.
constexpr double kDecodeShare = 0.8;

void report(const DecodeOptions& o, double fraction)
{
    if (o.progress)
        o.progress(std::clamp(fraction, 0.0, 1.0));
}

const juce::StringArray& juceExtensions()
{
    static const juce::StringArray e{".wav", ".wave", ".aif", ".aiff", ".aifc", ".flac", ".ogg"};
    return e;
}

const juce::StringArray& mediaFoundationExtensions()
{
    static const juce::StringArray e{".mp3", ".m4a", ".aac", ".mp4", ".alac"};
    return e;
}

// ---- JUCE readers ----

util::Result<RawAudio> decodeWithJuce(const juce::File& file, const DecodeOptions& options)
{
    juce::AudioFormatManager formats;
    formats.registerFormat(new juce::WavAudioFormat(), true);
    formats.registerFormat(new juce::AiffAudioFormat(), false);
    formats.registerFormat(new juce::FlacAudioFormat(), false);
    formats.registerFormat(new juce::OggVorbisAudioFormat(), false);

    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
        return util::Result<RawAudio>::fail("Not a readable audio file: " + file.getFileName().toStdString());
    if (reader->sampleRate <= 0.0 || reader->numChannels == 0 || reader->lengthInSamples < 0)
        return util::Result<RawAudio>::fail("Audio file has no usable audio: " + file.getFileName().toStdString());
    if (reader->lengthInSamples > std::numeric_limits<int>::max())
        return util::Result<RawAudio>::fail("Audio file is too long: " + file.getFileName().toStdString());

    RawAudio raw;
    raw.sampleRate = reader->sampleRate;
    raw.sourceChannels = static_cast<int>(reader->numChannels);
    raw.formatName = reader->getFormatName();
    const int keep = std::min(2, raw.sourceChannels);
    const auto length = static_cast<int>(reader->lengthInSamples);
    raw.channels.assign(static_cast<std::size_t>(keep), std::vector<float>(static_cast<std::size_t>(length)));

    juce::AudioBuffer<float> chunk(raw.sourceChannels, kChunkSamples);
    for (int pos = 0; pos < length; pos += kChunkSamples)
    {
        report(options, kDecodeShare * pos / std::max(1, length));
        if (cancelled(options))
            return util::Result<RawAudio>::fail("Cancelled");
        const int n = std::min(kChunkSamples, length - pos);
        if (!reader->read(&chunk, 0, n, pos, true, true))
            return util::Result<RawAudio>::fail("Read error in " + file.getFileName().toStdString());
        for (int ch = 0; ch < keep; ++ch)
            std::copy_n(chunk.getReadPointer(ch), n, raw.channels[static_cast<std::size_t>(ch)].begin() + pos);
    }
    return util::Result<RawAudio>::ok(std::move(raw));
}

// ---- Gapless trimming for packetised formats ----

void keepRange(RawAudio& raw, std::int64_t start, std::int64_t length)
{
    for (auto& ch : raw.channels)
    {
        const auto from = std::min<std::int64_t>(start, static_cast<std::int64_t>(ch.size()));
        const auto to = std::min<std::int64_t>(from + length, static_cast<std::int64_t>(ch.size()));
        ch.erase(ch.begin() + to, ch.end());
        ch.erase(ch.begin(), ch.begin() + from);
    }
}

// MP3: remove encoder delay/padding using the LAME header. M4A/AAC: cut the frame padding past the length the
// file declares (Media Foundation already skips the AAC priming samples).
void applyGaplessTrim(const juce::File& file, RawAudio& raw)
{
    if (raw.channels.empty())
        return;
    const auto decoded = static_cast<std::int64_t>(raw.channels[0].size());

    if (file.hasFileExtension(".mp3"))
    {
        juce::MemoryBlock head;
        if (auto in = file.createInputStream())
            in->readIntoMemoryBlock(head, 256 * 1024);
        const auto info = parseMp3Gapless({static_cast<const std::uint8_t*>(head.getData()), head.getSize()});
        if (!info)
            return; // no LAME header: keep everything
        if (const auto trim = mp3TrimRange(*info, decoded))
            keepRange(raw, trim->start, trim->length);
        return;
    }

    if (raw.containerSeconds > 0.0)
    {
        const auto declared = static_cast<std::int64_t>(std::llround(raw.containerSeconds * raw.sampleRate));
        if (declared > 0 && declared < decoded)
            keepRange(raw, 0, declared);
    }
}

// ---- Windows Media Foundation ----

#if JUCE_WINDOWS
using Microsoft::WRL::ComPtr;

std::string hresultMessage(const char* what, HRESULT hr)
{
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return buffer;
}

// COM + Media Foundation for the lifetime of one decode on this thread.
struct MediaFoundationScope
{
    MediaFoundationScope()
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comOwned = SUCCEEDED(co); // S_OK or S_FALSE; RPC_E_CHANGED_MODE = already initialised differently (fine)
        startup = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    }
    ~MediaFoundationScope()
    {
        if (SUCCEEDED(startup))
            MFShutdown();
        if (comOwned)
            CoUninitialize();
    }
    bool comOwned = false;
    HRESULT startup = E_FAIL;
};

util::Result<RawAudio> decodeWithMediaFoundation(const juce::File& file, const DecodeOptions& options)
{
    using R = util::Result<RawAudio>;
    MediaFoundationScope mf;
    if (FAILED(mf.startup))
        return R::fail(hresultMessage("Media Foundation startup", mf.startup) +
                       " - is Media Foundation installed on this Windows?");

    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(file.getFullPathName().toWideCharPointer(), nullptr, &reader);
    if (FAILED(hr))
        return R::fail("Not a readable audio file: " + file.getFileName().toStdString() + " (" +
                       hresultMessage("MFCreateSourceReaderFromURL", hr) + ")");

    const auto audioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(hr = reader->SetStreamSelection(audioStream, TRUE)))
        return R::fail("No audio stream in " + file.getFileName().toStdString());

    // Ask for 32-bit float PCM at the file's own rate and channel count; MF inserts the decoder.
    ComPtr<IMFMediaType> wanted;
    MFCreateMediaType(&wanted);
    wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    wanted->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(hr = reader->SetCurrentMediaType(audioStream, nullptr, wanted.Get())))
        return R::fail("Can't decode " + file.getFileName().toStdString() + " (" +
                       hresultMessage("SetCurrentMediaType", hr) + ")");

    ComPtr<IMFMediaType> actual;
    reader->GetCurrentMediaType(audioStream, &actual);
    UINT32 channels = 0, rate = 0;
    actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    if (channels == 0 || rate == 0)
        return R::fail("Audio stream has no usable format: " + file.getFileName().toStdString());

    RawAudio raw;
    raw.sampleRate = rate;
    raw.sourceChannels = static_cast<int>(channels);
    const auto ext = file.getFileExtension().toLowerCase();
    raw.formatName = (ext == ".mp3"   ? "MP3"
                      : ext == ".aac" ? "AAC"
                                      : "MPEG-4 audio") +
                     juce::String(" (Media Foundation)");
    const int keep = std::min(2, raw.sourceChannels);
    raw.channels.assign(static_cast<std::size_t>(keep), {});

    // Reserve from the container duration (100 ns units) to avoid repeated growth.
    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION,
                                                   &duration)))
    {
        raw.containerSeconds = static_cast<double>(duration.uhVal.QuadPart) * 1e-7;
        const auto expected = static_cast<std::size_t>(raw.containerSeconds * rate) + 4096;
        for (auto& ch : raw.channels)
            ch.reserve(expected);
    }
    PropVariantClear(&duration);

    for (;;)
    {
        if (cancelled(options))
            return R::fail("Cancelled");

        DWORD streamIndex = 0, flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        if (raw.containerSeconds > 0.0)
            report(options, kDecodeShare * (static_cast<double>(timestamp) * 1e-7) / raw.containerSeconds);
        if (FAILED(hr = reader->ReadSample(audioStream, 0, &streamIndex, &flags, &timestamp, &sample)))
            return R::fail("Decode error in " + file.getFileName().toStdString() + " (" +
                           hresultMessage("ReadSample", hr) + ")");
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
            break;
        if (sample == nullptr)
            continue;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
            continue;
        BYTE* data = nullptr;
        DWORD bytes = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &bytes)))
            continue;

        const auto* frames = reinterpret_cast<const float*>(data);
        const std::size_t frameCount = bytes / (sizeof(float) * channels);
        for (int ch = 0; ch < keep; ++ch)
        {
            auto& out = raw.channels[static_cast<std::size_t>(ch)];
            for (std::size_t i = 0; i < frameCount; ++i)
                out.push_back(frames[i * channels + static_cast<std::size_t>(ch)]);
        }
        buffer->Unlock();
    }
    applyGaplessTrim(file, raw);
    return R::ok(std::move(raw));
}
#endif
} // namespace

const juce::StringArray& supportedTrackExtensions()
{
    static const juce::StringArray all = []
    {
        juce::StringArray e = juceExtensions();
#if JUCE_WINDOWS
        e.addArray(mediaFoundationExtensions());
#endif
        return e;
    }();
    return all;
}

bool isSupportedTrackFile(const juce::File& file)
{
    return supportedTrackExtensions().contains(file.getFileExtension().toLowerCase());
}

DecodeResult decodeTrack(const juce::File& file, const DecodeOptions& options)
{
    if (!file.existsAsFile())
        return DecodeResult::fail("File not found: " + file.getFullPathName().toStdString());

    const auto ext = file.getFileExtension().toLowerCase();
    util::Result<RawAudio> raw = util::Result<RawAudio>::fail("Unsupported file type: " + ext.toStdString());
    if (juceExtensions().contains(ext))
        raw = decodeWithJuce(file, options);
#if JUCE_WINDOWS
    else if (mediaFoundationExtensions().contains(ext))
        raw = decodeWithMediaFoundation(file, options);
#endif
    if (!raw)
        return DecodeResult::fail(raw.error());

    auto& audio = raw.value();
    if (audio.channels.empty() || audio.channels[0].empty())
        return DecodeResult::fail("Audio file is empty: " + file.getFileName().toStdString());
    if (audio.channels.size() == 1)
        audio.channels.push_back(audio.channels[0]); // mono -> both sides

    auto track = std::make_unique<TrackBuffer>();
    track->file = file;
    track->formatName = audio.formatName;
    track->sourceSampleRate = audio.sampleRate;
    track->sourceChannels = audio.sourceChannels;
    track->sourceLengthSamples = static_cast<std::int64_t>(audio.channels[0].size());
    track->containerDurationSeconds = audio.containerSeconds;

    const bool convert = options.targetSampleRate > 0.0 && std::abs(options.targetSampleRate - audio.sampleRate) > 1e-6;
    if (convert)
    {
        if (cancelled(options))
            return DecodeResult::fail("Cancelled");
        // Both channels in parallel.
        auto right =
            std::async(std::launch::async,
                       [&] { return dsp::resample(audio.channels[1], audio.sampleRate, options.targetSampleRate); });
        audio.channels[0] = dsp::resample(audio.channels[0], audio.sampleRate, options.targetSampleRate);
        audio.channels[1] = right.get();
        track->sampleRate = options.targetSampleRate;
        report(options, 0.95);
    }
    else
    {
        track->sampleRate = audio.sampleRate;
    }

    const auto length = audio.channels[0].size();
    if (length > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return DecodeResult::fail("Audio file is too long: " + file.getFileName().toStdString());
    track->audio.setSize(2, static_cast<int>(length));
    for (int ch = 0; ch < 2; ++ch)
        std::copy(audio.channels[static_cast<std::size_t>(ch)].begin(),
                  audio.channels[static_cast<std::size_t>(ch)].end(), track->audio.getWritePointer(ch));

    report(options, 1.0);
    return DecodeResult::ok(std::move(track));
}

} // namespace devildj::engine
