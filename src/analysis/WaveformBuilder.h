#pragma once

#include <array>
#include <vector>

namespace devildj::analysis
{

// Waveform data for display (P1-14). Built once per track on the loader thread (later: the analysis queue).
//
// Detail: 150 points per second (samplesPerPoint = round(sampleRate / 150)); point p covers samples
// [p * samplesPerPoint, (p + 1) * samplesPerPoint). Per point and band: peak (max |x|) and RMS.
// Bands, on the mono sum 0.5 (L + R): low < 250 Hz, mid 250 Hz - 2.5 kHz, high > 2.5 kHz, split with
// Linkwitz-Riley 4th-order crossovers (the display doesn't need the mixer's steeper LR8).
// Overview: 1,024 points over the whole track, per band peak and RMS, plus the full-band peak / RMS.
struct Waveform
{
    static constexpr double kPointsPerSecond = 150.0;
    static constexpr int kOverviewPoints = 1024;
    enum Band
    {
        Low,
        Mid,
        High,
    };
    static constexpr int kBands = 3;

    int samplesPerPoint = 0;
    double sampleRate = 0.0;
    std::array<std::vector<float>, kBands> peak, rms; // detail
    std::array<std::vector<float>, kBands> overviewPeak, overviewRms;
    std::vector<float> overviewTotalPeak, overviewTotalRms; // full band

    int numPoints() const noexcept { return static_cast<int>(peak[0].size()); }
    bool empty() const noexcept { return peak[0].empty(); }
    double pointsPerSecond() const noexcept { return samplesPerPoint > 0 ? sampleRate / samplesPerPoint : 0.0; }
};

Waveform buildWaveform(const float* left, const float* right, int numSamples, double sampleRate);

} // namespace devildj::analysis
