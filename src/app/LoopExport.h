#pragma once

#include "engine/TrackBuffer.h"

#include <juce_core/juce_core.h>

#include <memory>

namespace devildj::app
{

// P3-03: a loop of a loaded track as a WAV file (24-bit, stereo, the track buffer's rate), samples [start, end)
// of the buffer. Message thread (writes a file). Returns an error, or empty.
juce::String writeLoopWav(const engine::TrackBuffer& track, int start, int end, const juce::File& file,
                          int bitsPerSample = 24);

// P3-14: samples [start, end) of a track as a new buffer of their own (for a sampler slot): the same frames, bit for
// bit, at the same rate. nullptr if the range is empty. Message thread (allocates).
std::unique_ptr<engine::TrackBuffer> copyRange(const engine::TrackBuffer& track, int start, int end);

} // namespace devildj::app
