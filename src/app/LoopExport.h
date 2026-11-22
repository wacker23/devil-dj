#pragma once

#include "engine/TrackBuffer.h"

#include <juce_core/juce_core.h>

namespace devildj::app
{

// P3-03: a loop of a loaded track as a WAV file (24-bit, stereo, the track buffer's rate), samples [start, end)
// of the buffer. Message thread (writes a file). Returns an error, or empty.
juce::String writeLoopWav(const engine::TrackBuffer& track, int start, int end, const juce::File& file);

} // namespace devildj::app
