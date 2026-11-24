#include "engine/StemLoader.h"

#include "dsp/SincResampler.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace devildj::engine
{

namespace
{
constexpr const char* kStemFiles[StemBuffer::kStems] = {"drums.flac", "bass.flac", "other.flac", "vocals.flac"};
}

std::int16_t toInt16(float x) noexcept
{
    const float v = std::round(x * 32768.0f);
    return static_cast<std::int16_t>(std::clamp(v, -32768.0f, 32767.0f));
}

util::Result<std::unique_ptr<StemBuffer>> loadStems(const juce::File& folder, const TrackBuffer& track,
                                                    const std::atomic<bool>* cancel)
{
    using R = util::Result<std::unique_ptr<StemBuffer>>;
    auto stems = std::make_unique<StemBuffer>();
    stems->length = track.numSamples();
    stems->sampleRate = track.sampleRate;
    stems->track = &track;
    juce::FlacAudioFormat flac;
    std::vector<float> channel;
    for (int s = 0; s < StemBuffer::kStems; ++s)
    {
        if (cancel != nullptr && cancel->load())
            return R::fail("cancelled");
        const auto file = folder.getChildFile(kStemFiles[s]);
        if (!file.existsAsFile())
            return R::fail("no stem file " + file.getFullPathName().toStdString());
        std::unique_ptr<juce::AudioFormatReader> reader(flac.createReaderFor(file.createInputStream().release(), true));
        if (reader == nullptr)
            return R::fail("can't read " + file.getFullPathName().toStdString());
        const auto n = static_cast<int>(reader->lengthInSamples);
        juce::AudioBuffer<float> audio(2, n);
        if (!reader->read(&audio, 0, n, 0, true, true))
            return R::fail("can't read " + file.getFullPathName().toStdString());
        if (reader->numChannels < 2)
            audio.copyFrom(1, 0, audio, 0, 0, n);
        for (int c = 0; c < 2; ++c)
        {
            const float* data = audio.getReadPointer(c);
            const float* from = data;
            if (std::abs(reader->sampleRate - track.sampleRate) > 0.5)
            {
                channel = dsp::resample(std::span<const float>(data, static_cast<std::size_t>(n)), reader->sampleRate,
                                        track.sampleRate);
                from = channel.data();
            }
            const auto have = std::abs(reader->sampleRate - track.sampleRate) > 0.5 ? channel.size()
                                                                                    : static_cast<std::size_t>(n);
            auto& out = stems->samples[static_cast<std::size_t>(s * 2 + c)];
            out.assign(static_cast<std::size_t>(stems->length), 0);
            const auto count = std::min(have, out.size());
            for (std::size_t i = 0; i < count; ++i)
                out[i] = toInt16(from[i]);
        }
    }
    // Peaks for the waveform view.
    const auto points = static_cast<std::size_t>((stems->length + StemBuffer::kPeakSamples - 1) / StemBuffer::kPeakSamples);
    for (int s = 0; s < StemBuffer::kStems; ++s)
    {
        auto& p = stems->peaks[static_cast<std::size_t>(s)];
        p.assign(points, 0.0f);
        for (int c = 0; c < 2; ++c)
        {
            const auto* data = stems->channel(s, c);
            for (int i = 0; i < stems->length; ++i)
            {
                auto& peak = p[static_cast<std::size_t>(i / StemBuffer::kPeakSamples)];
                peak = std::max(peak, std::abs(static_cast<float>(data[i])) * (1.0f / 32768.0f));
            }
        }
    }
    return R::ok(std::move(stems));
}

} // namespace devildj::engine
