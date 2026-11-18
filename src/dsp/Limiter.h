#pragma once

#include <vector>

namespace devildj::dsp
{

// Brick-wall look-ahead peak limiter for the master bus (ARCHITECTURE §4): 1.5 ms look-ahead, 5 ms release,
// ceiling -0.3 dBFS. Stereo-linked.
//
// For every incoming sample the gain it needs is ceiling / peak. That requirement is min-held over the
// look-ahead window (+1), released towards 1 (never above the held minimum), then averaged over the same window.
// The average of values that are each <= the requirement of the sample now leaving the delay line is itself
// <= that requirement, so the output never exceeds the ceiling, and the gain still moves smoothly (a linear
// fade over the look-ahead instead of a step). A final clamp catches float rounding.
// Latency: window samples (72 at 48 kHz). prepare() allocates; process() doesn't.
class Limiter
{
public:
    static constexpr double kLookaheadSeconds = 0.0015;
    static constexpr double kReleaseSeconds = 0.005;
    static constexpr double kCeilingDb = -0.3;

    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;
    void process(float* left, float* right, int numSamples) noexcept;

    int latencySamples() const noexcept { return window; }
    float ceiling() const noexcept { return ceilingGain; }
    // Largest gain reduction during the last process() call, in dB (>= 0).
    float lastGainReductionDb() const noexcept { return lastReductionDb; }

private:
    int window = 1;
    float ceilingGain = 1.0f;
    double releaseCoeff = 0.0;
    std::vector<float> delayL, delayR; // window samples each
    std::vector<double> held;          // released, min-held gains (window), averaged for the output gain
    std::vector<double> required;      // raw per-sample requirement (window + 1), for the min-hold
    int pos = 0;
    int requiredPos = 0;
    double envelope = 1.0;
    float lastReductionDb = 0.0f;
};

} // namespace devildj::dsp
