#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "analysis/WaveformBuilder.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace devildj::engine
{

// A fully decoded track in memory (ARCHITECTURE §3.4, §3.6): float stereo at the device sample rate.
// Created on a loader thread, then read-only. The audio thread only ever reads it through a raw pointer;
// ownership stays in the BufferRegistry on the message thread.
struct TrackBuffer
{
    TrackBuffer() noexcept { liveBuffers.fetch_add(1, std::memory_order_relaxed); }
    ~TrackBuffer() { liveBuffers.fetch_sub(1, std::memory_order_relaxed); }
    TrackBuffer(const TrackBuffer&) = delete;
    TrackBuffer& operator=(const TrackBuffer&) = delete;

    juce::AudioBuffer<float> audio; // 2 channels (mono sources are duplicated), length = numSamples()
    double sampleRate = 0.0;        // rate of 'audio' (the device rate when resampled at load)

    // About the source file.
    juce::File file;
    juce::String formatName; // e.g. "WAV", "FLAC", "MP3 (Media Foundation)"
    double sourceSampleRate = 0.0;
    int sourceChannels = 0;
    std::int64_t sourceLengthSamples = 0;
    double containerDurationSeconds = 0.0; // what the file declares (MP3/M4A via Media Foundation); 0 = unknown
    juce::String contentHash;              // xxHash3 of file size + first/last 1 MB (set by the loader)

    // Set by the loader: tags (P1-13) and the 3-band waveform with its overview (P1-14).
    juce::String title, artist; // empty if the file has no tags
    double tagBpm = 0.0;        // BPM tag; 0 = none (analysis comes in Phase 2)
    analysis::Waveform waveform;

    int numSamples() const noexcept { return audio.getNumSamples(); }
    double durationSeconds() const noexcept { return sampleRate > 0.0 ? numSamples() / sampleRate : 0.0; }
    std::size_t memoryBytes() const noexcept
    {
        return static_cast<std::size_t>(audio.getNumChannels()) * static_cast<std::size_t>(numSamples()) *
               sizeof(float);
    }

    // Number of TrackBuffers alive in the process (tests and the debug overlay check memory is released).
    static int liveCount() noexcept { return liveBuffers.load(std::memory_order_relaxed); }

private:
    inline static std::atomic<int> liveBuffers{0};
};

} // namespace devildj::engine
