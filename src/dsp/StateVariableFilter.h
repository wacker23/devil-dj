#pragma once

// Topology-preserving-transform (trapezoidal) state-variable filter. Stays stable and click-free when the
// cutoff changes every sample, which is why the channel filter knob uses it. One channel of state; low-pass,
// band-pass and high-pass outputs from the same step. Header-only, no allocation.

#include <algorithm>
#include <cmath>
#include <numbers>

namespace devildj::dsp
{

struct SvfCoeffs
{
    double a1 = 1.0, a2 = 0.0, a3 = 0.0, k = std::numbers::sqrt2;

    static SvfCoeffs make(double hz, double sampleRate, double q) noexcept
    {
        const double fc = std::clamp(hz, 1.0, 0.45 * sampleRate);
        const double g = std::tan(std::numbers::pi * fc / sampleRate);
        SvfCoeffs c;
        c.k = 1.0 / std::max(q, 0.1);
        c.a1 = 1.0 / (1.0 + g * (g + c.k));
        c.a2 = g * c.a1;
        c.a3 = g * c.a2;
        return c;
    }
};

struct SvfState
{
    double ic1 = 0.0, ic2 = 0.0;

    struct Out
    {
        double low, band, high;
    };

    Out process(const SvfCoeffs& c, double x) noexcept
    {
        const double v3 = x - ic2;
        const double v1 = c.a1 * ic1 + c.a2 * v3;
        const double v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        return {v2, v1, x - c.k * v1 - v2};
    }
    void reset() noexcept { ic1 = ic2 = 0.0; }
};

} // namespace devildj::dsp
