#pragma once

#include "engine/TrackBuffer.h"
#include "util/Result.h"

#include <atomic>
#include <memory>

namespace devildj::engine
{

// Decodes audio files into a TrackBuffer (P1-04). Not for the audio thread: it allocates and does file I/O.
//
//   WAV, AIFF, FLAC, Ogg Vorbis    -> JUCE's readers
//   MP3, AAC, M4A (AAC or ALAC)    -> Windows Media Foundation (IMFSourceReader)
//
// Output is float stereo: mono sources are duplicated to both channels, sources with more than two channels
// use their first two. If targetSampleRate is set and differs from the file's rate, the audio is converted
// with the high-quality sinc resampler (dsp/SincResampler).
struct DecodeOptions
{
    double targetSampleRate = 0.0;             // 0 = keep the file's rate
    const std::atomic<bool>* cancel = nullptr; // checked between chunks; set to stop early
};

util::Result<std::unique_ptr<TrackBuffer>> decodeTrack(const juce::File& file, const DecodeOptions& options = {});

// File extensions decodeTrack() accepts, lower case with the dot (".wav", ".mp3", ...).
const juce::StringArray& supportedTrackExtensions();
bool isSupportedTrackFile(const juce::File& file);

} // namespace devildj::engine
