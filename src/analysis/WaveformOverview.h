#pragma once

#include <vector>

namespace devildj::analysis
{

// Whole-track overview for the deck view (P1-13): per bucket, the peak (max |sample| over both channels) and
// the RMS. P1-14's WaveformBuilder adds the 3-band data. Runs on the loader thread.
struct WaveformOverview
{
    static constexpr int kDefaultPoints = 1024;
    std::vector<float> peak; // 0..1+
    std::vector<float> rms;

    int size() const noexcept { return static_cast<int>(peak.size()); }
};

// left / right: numSamples each (right may equal left for mono). points > 0.
WaveformOverview buildOverview(const float* left, const float* right, int numSamples,
                               int points = WaveformOverview::kDefaultPoints);

} // namespace devildj::analysis
