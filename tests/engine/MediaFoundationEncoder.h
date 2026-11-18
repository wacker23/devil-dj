#pragma once

// Test helper (Windows): encodes a stereo float buffer to AAC or ALAC in an .m4a file with Media Foundation's
// built-in encoders, so the decoder tests get M4A files without committing any (P1-04).

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

namespace devildj::test
{

// True if Media Foundation works on this machine (Windows Server images may not have it installed).
inline bool mediaFoundationAvailable()
{
    static const bool available = []
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (SUCCEEDED(hr))
            MFShutdown();
        if (SUCCEEDED(co))
            CoUninitialize();
        return SUCCEEDED(hr);
    }();
    return available;
}

enum class M4aCodec
{
    Aac,
    Alac,
};

// Returns an empty string on success, otherwise why it failed (e.g. the encoder isn't installed).
inline juce::String encodeM4a(const juce::File& out, const juce::AudioBuffer<float>& audio, int sampleRate,
                              M4aCodec codec)
{
    using Microsoft::WRL::ComPtr;
    const auto fail = [](const char* what, HRESULT hr)
    { return juce::String(what) + " failed (0x" + juce::String::toHexString(static_cast<int>(hr)) + ")"; };

    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(co);
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr))
    {
        if (comOwned)
            CoUninitialize();
        return fail("MFStartup", hr);
    }

    juce::String error;
    {
        out.deleteFile();
        ComPtr<IMFAttributes> attributes;
        MFCreateAttributes(&attributes, 1);
        attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);

        ComPtr<IMFSinkWriter> writer;
        const int channels = 2;
        if (FAILED(hr = MFCreateSinkWriterFromURL(out.getFullPathName().toWideCharPointer(), nullptr, attributes.Get(),
                                                  &writer)))
            error = fail("MFCreateSinkWriterFromURL", hr);

        ComPtr<IMFMediaType> outType, inType;
        DWORD stream = 0;
        if (error.isEmpty())
        {
            MFCreateMediaType(&outType);
            outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            outType->SetGUID(MF_MT_SUBTYPE, codec == M4aCodec::Aac ? MFAudioFormat_AAC : MFAudioFormat_ALAC);
            outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
            outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(sampleRate));
            outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            if (codec == M4aCodec::Aac)
                outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000); // 192 kbit/s
            if (FAILED(hr = writer->AddStream(outType.Get(), &stream)))
                error = fail("AddStream (encoder not available?)", hr);
        }
        if (error.isEmpty())
        {
            MFCreateMediaType(&inType);
            inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
            inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(sampleRate));
            inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2 * channels);
            inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(sampleRate * 2 * channels));
            if (FAILED(hr = writer->SetInputMediaType(stream, inType.Get(), nullptr)))
                error = fail("SetInputMediaType", hr);
        }
        if (error.isEmpty() && FAILED(hr = writer->BeginWriting()))
            error = fail("BeginWriting", hr);

        constexpr int kFrames = 1024;
        for (int pos = 0; error.isEmpty() && pos < audio.getNumSamples(); pos += kFrames)
        {
            const int n = std::min(kFrames, audio.getNumSamples() - pos);
            ComPtr<IMFMediaBuffer> buffer;
            MFCreateMemoryBuffer(static_cast<DWORD>(n * channels * 2), &buffer);
            BYTE* data = nullptr;
            buffer->Lock(&data, nullptr, nullptr);
            auto* pcm = reinterpret_cast<std::int16_t*>(data);
            for (int i = 0; i < n; ++i)
                for (int ch = 0; ch < channels; ++ch)
                    pcm[i * channels + ch] = static_cast<std::int16_t>(
                        juce::jlimit(-32768, 32767, juce::roundToInt(audio.getSample(ch, pos + i) * 32767.0f)));
            buffer->Unlock();
            buffer->SetCurrentLength(static_cast<DWORD>(n * channels * 2));

            ComPtr<IMFSample> sample;
            MFCreateSample(&sample);
            sample->AddBuffer(buffer.Get());
            sample->SetSampleTime(static_cast<LONGLONG>(pos * 10'000'000.0 / sampleRate));
            sample->SetSampleDuration(static_cast<LONGLONG>(n * 10'000'000.0 / sampleRate));
            if (FAILED(hr = writer->WriteSample(stream, sample.Get())))
                error = fail("WriteSample", hr);
        }
        if (error.isEmpty() && FAILED(hr = writer->Finalize()))
            error = fail("Finalize", hr);
    }

    MFShutdown();
    if (comOwned)
        CoUninitialize();
    return error;
}

} // namespace devildj::test

#endif
