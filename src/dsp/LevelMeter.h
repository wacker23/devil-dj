#pragma once

// Stereo peak + RMS meter for display. Peaks are held per block and fall at 20 dB/s, so a UI that reads the
// value at 60 Hz still sees every peak; RMS integrates over 300 ms. Linear values. Header-only, no allocation.

#include <algorithm>
#include <cmath>

namespace devildj::dsp
{

class LevelMeter
{
public:
    static constexpr double kPeakFallDbPerSecond = 20.0;
    static constexpr double kRmsSeconds = 0.3;

    void prepare(double sampleRate) noexcept
    {
        rate = sampleRate;
        rmsCoeff = 1.0 - std::exp(-1.0 / (kRmsSeconds * sampleRate));
        reset();
    }

    void reset() noexcept
    {
        peakL = peakR = 0.0f;
        meanSquare = 0.0;
    }

    void process(const float* left, const float* right, int numSamples) noexcept
    {
        fall(numSamples);
        float pl = 0.0f, pr = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            pl = std::max(pl, std::abs(left[i]));
            pr = std::max(pr, std::abs(right[i]));
            const double ms = 0.5 * (static_cast<double>(left[i]) * left[i] + static_cast<double>(right[i]) * right[i]);
            meanSquare += rmsCoeff * (ms - meanSquare);
        }
        peakL = std::max(peakL, pl);
        peakR = std::max(peakR, pr);
    }

    // Nothing playing: peaks fall and RMS decays as if fed silence.
    void processSilence(int numSamples) noexcept
    {
        fall(numSamples);
        meanSquare *= std::pow(1.0 - rmsCoeff, numSamples);
        if (meanSquare < 1e-12)
            meanSquare = 0.0;
    }

    float peakLeft() const noexcept { return peakL; }
    float peakRight() const noexcept { return peakR; }
    float rms() const noexcept { return static_cast<float>(std::sqrt(meanSquare)); }

private:
    void fall(int numSamples) noexcept
    {
        const auto g = static_cast<float>(std::pow(10.0, -kPeakFallDbPerSecond * numSamples / rate / 20.0));
        peakL = peakL * g < 1e-6f ? 0.0f : peakL * g;
        peakR = peakR * g < 1e-6f ? 0.0f : peakR * g;
    }

    double rate = 48000.0, rmsCoeff = 0.0, meanSquare = 0.0;
    float peakL = 0.0f, peakR = 0.0f;
};

} // namespace devildj::dsp
