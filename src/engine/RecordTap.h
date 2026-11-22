#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace devildj::engine
{

// The record tap (P3-16; ARCHITECTURE §3.9): the audio thread copies the bus being recorded - the master after its
// limiter, or the headphones - into this lock-free ring (one writer: the audio thread; one reader: the recorder's
// writer thread). About 2 s of room; if the reader falls that far behind, frames are dropped and counted, never
// waited for. Stereo, interleaved.
class RecordTap
{
public:
    enum class Source : int
    {
        Master = 0,
        Headphones = 1,
    };

    // Not real-time safe (allocates): room for `seconds` at `sampleRate` (rounded up to a power of two frames).
    void prepare(double sampleRate, double seconds = 2.0)
    {
        std::size_t frames = 1;
        while (frames < static_cast<std::size_t>(sampleRate * seconds))
            frames <<= 1;
        ring.assign(2 * frames, 0.0f);
        mask = frames - 1;
        writeIndex.store(0);
        readIndex.store(0);
        dropped.store(0);
    }

    // Message thread: start / stop taking frames, and which bus.
    void setEnabled(bool on) noexcept { enabled.store(on, std::memory_order_release); }
    bool isEnabled() const noexcept { return enabled.load(std::memory_order_acquire); }
    void setSource(Source s) noexcept { source.store(static_cast<int>(s), std::memory_order_release); }
    Source getSource() const noexcept { return static_cast<Source>(source.load(std::memory_order_acquire)); }

    // Audio thread: append a block (when enabled). Real-time safe.
    void write(const float* left, const float* right, int numSamples) noexcept
    {
        if (!isEnabled() || ring.empty())
            return;
        const auto w = writeIndex.load(std::memory_order_relaxed);
        const auto r = readIndex.load(std::memory_order_acquire);
        const std::size_t capacity = mask + 1;
        const std::size_t free = capacity - static_cast<std::size_t>(w - r);
        const std::size_t n = std::min(free, static_cast<std::size_t>(numSamples));
        for (std::size_t i = 0; i < n; ++i)
        {
            const std::size_t at = 2 * ((w + i) & mask);
            ring[at] = left[i];
            ring[at + 1] = right[i];
        }
        writeIndex.store(w + n, std::memory_order_release);
        if (n < static_cast<std::size_t>(numSamples))
            dropped.fetch_add(static_cast<std::uint64_t>(numSamples) - n, std::memory_order_relaxed);
    }

    // Writer thread: up to `maxFrames` into left / right; returns how many.
    int read(float* left, float* right, int maxFrames) noexcept
    {
        const auto r = readIndex.load(std::memory_order_relaxed);
        const auto w = writeIndex.load(std::memory_order_acquire);
        const auto n = static_cast<int>(std::min<std::uint64_t>(w - r, static_cast<std::uint64_t>(maxFrames)));
        for (int i = 0; i < n; ++i)
        {
            const std::size_t at = 2 * ((r + static_cast<std::uint64_t>(i)) & mask);
            left[i] = ring[at];
            right[i] = ring[at + 1];
        }
        readIndex.store(r + static_cast<std::uint64_t>(n), std::memory_order_release);
        return n;
    }
    int available() const noexcept
    {
        return static_cast<int>(writeIndex.load(std::memory_order_acquire) - readIndex.load(std::memory_order_relaxed));
    }
    std::uint64_t droppedFrames() const noexcept { return dropped.load(std::memory_order_relaxed); }

private:
    std::vector<float> ring;
    std::size_t mask = 0;
    std::atomic<std::uint64_t> writeIndex{0}, readIndex{0}, dropped{0};
    std::atomic<bool> enabled{false};
    std::atomic<int> source{0};
};

} // namespace devildj::engine
