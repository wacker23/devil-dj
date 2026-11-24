#pragma once

#include "engine/StemBuffer.h"
#include "util/Result.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>

namespace devildj::engine
{

// Reads a track's stems from the stem cache (P6-04): the folder app::StemService writes
// (drums/bass/other/vocals.flac, 16-bit stereo at 44.1 kHz) into a StemBuffer for `track`: at the track buffer's
// rate (the analysis sinc resampler when they differ) and exactly its length (cut or padded with silence), so the
// stems and the track share one timeline. Loader threads only (allocates, reads files). `cancel` is checked
// between stems.
util::Result<std::unique_ptr<StemBuffer>> loadStems(const juce::File& folder, const TrackBuffer& track,
                                                    const std::atomic<bool>* cancel = nullptr);

// Float to 16-bit with rounding and clipping (what the stems are kept as).
std::int16_t toInt16(float x) noexcept;

} // namespace devildj::engine
