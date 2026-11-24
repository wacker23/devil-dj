#pragma once

#include "engine/TrackBuffer.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace devildj::engine
{

// A track's four stems in memory (P6-04): drums, bass, other, vocals (the order analysis::demucs writes them),
// 16-bit stereo at the device rate, the same timeline as the TrackBuffer they were made from. Created on a
// loader thread from the stem cache's FLACs, then read-only; owned by the BufferRegistry like a TrackBuffer and
// handed to the engine by pointer (ARCHITECTURE §3.6).
struct StemBuffer
{
    static constexpr int kStems = 4;

    StemBuffer() noexcept { liveBuffers.fetch_add(1, std::memory_order_relaxed); }
    ~StemBuffer() { liveBuffers.fetch_sub(1, std::memory_order_relaxed); }
    StemBuffer(const StemBuffer&) = delete;
    StemBuffer& operator=(const StemBuffer&) = delete;

    std::array<std::vector<std::int16_t>, kStems * 2> samples; // [stem * 2 + channel]
    int length = 0;                                            // samples a channel
    double sampleRate = 0.0;
    const TrackBuffer* track = nullptr; // the track these belong to (the engine checks it's still on the deck)
    // For the stem waveform view: each stem's peak (0..1, both channels) per kPeakSamples samples.
    static constexpr int kPeakSamples = 256;
    std::array<std::vector<float>, kStems> peaks;

    const std::int16_t* channel(int stem, int ch) const noexcept
    {
        return samples[static_cast<std::size_t>(stem * 2 + ch)].data();
    }

    std::size_t memoryBytes() const noexcept
    {
        std::size_t total = 0;
        for (const auto& s : samples)
            total += s.size() * sizeof(std::int16_t);
        for (const auto& p : peaks)
            total += p.size() * sizeof(float);
        return total;
    }

    static int liveCount() noexcept { return liveBuffers.load(std::memory_order_relaxed); }

private:
    inline static std::atomic<int> liveBuffers{0};
};

} // namespace devildj::engine
