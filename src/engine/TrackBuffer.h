#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <cstdint>

namespace devildj::engine
{

// A fully decoded track in memory (ARCHITECTURE §3.4, §3.6): float stereo at the device sample rate.
// Created on a loader thread, then read-only. The audio thread only ever reads it through a raw pointer.
struct TrackBuffer
{
    juce::AudioBuffer<float> audio; // 2 channels (mono sources are duplicated), length = numSamples()
    double sampleRate = 0.0;        // rate of 'audio' (the device rate when resampled at load)

    // About the source file.
    juce::File file;
    juce::String formatName; // e.g. "WAV", "FLAC", "MP3 (Media Foundation)"
    double sourceSampleRate = 0.0;
    int sourceChannels = 0;
    std::int64_t sourceLengthSamples = 0;
    double containerDurationSeconds = 0.0; // what the file declares (MP3/M4A via Media Foundation); 0 = unknown

    int numSamples() const noexcept { return audio.getNumSamples(); }
    double durationSeconds() const noexcept { return sampleRate > 0.0 ? numSamples() / sampleRate : 0.0; }
};

} // namespace devildj::engine
