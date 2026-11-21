#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace devildj::dsp
{

// A stereo buffer between two audio threads whose sound cards run on their own clocks (P2-25: the master on one
// device, the headphone cue on another). The writer (the engine's callback) puts blocks in; the reader (the second
// device's callback) takes what it needs, resampling by a ratio that follows the writer / reader rates and is
// nudged (at most +-0.5 %) so the buffer stays near its target fill - the two clocks drift apart by up to a few
// hundred ppm, so without that it would run dry or overflow within minutes.
//
//   - Lock-free, one writer thread and one reader thread; nothing allocates after construction.
//   - The reader starts silent and plays once the target fill is reached (stale audio beyond the target is
//     skipped, so the delay is the target); if it runs dry it goes silent and starts again.
//   - The writer drops a block that doesn't fit (the reader isn't running) and counts it.
//   - Interpolation: 4-point cubic (Hermite); the ratio is steered by the fill smoothed over ~2 s, so the block
//     sizes' sawtooth doesn't wobble the pitch.
class DriftBuffer
{
public:
    explicit DriftBuffer(int capacityFrames = 1 << 15); // rounded up to a power of two

    // Rates and block sizes of both sides (any thread; the reader picks them up). The target fill follows the
    // blocks: 1.5 x (writer block + reader block) + 128 frames.
    void setWriter(double sampleRate, int blockSize) noexcept;
    void setReader(double sampleRate, int blockSize) noexcept;

    void write(const float* left, const float* right, int numFrames) noexcept; // writer thread
    void read(float* left, float* right, int numFrames) noexcept;              // reader thread (always fills)
    void restart() noexcept; // reader thread, or while the reader isn't running: start priming again

    // Readouts (any thread).
    std::uint32_t underruns() const noexcept { return underrunCount.load(std::memory_order_relaxed); }
    std::uint32_t overruns() const noexcept { return overrunCount.load(std::memory_order_relaxed); }
    double fillFrames() const noexcept { return lastFill.load(std::memory_order_relaxed); }
    double targetFrames() const noexcept;
    double ratioCorrection() const noexcept { return correctionShown.load(std::memory_order_relaxed); } // 1 + c
    bool playing() const noexcept { return !priming.load(std::memory_order_relaxed); }

private:
    float sample(int channel, std::int64_t frame) const noexcept;

    std::vector<float> data; // interleaved L/R
    std::int64_t mask = 0;
    std::atomic<std::int64_t> written{0};  // frames written (writer)
    std::atomic<std::int64_t> readFloor{0}; // oldest frame the reader still needs (reader)
    std::atomic<double> writerRate{48000.0}, readerRate{48000.0};
    std::atomic<int> writerBlock{512}, readerBlock{512};
    std::atomic<bool> restartRequested{false};

    // Reader state.
    double position = 0.0; // fractional frame position of the next output sample
    std::int64_t resumeFrom = 0; // priming waits for the target fill of frames written after this
    std::atomic<bool> priming{true};
    double smoothedError = 0.0;

    std::atomic<std::uint32_t> underrunCount{0}, overrunCount{0};
    std::atomic<double> lastFill{0.0}, correctionShown{1.0};
};

} // namespace devildj::dsp
