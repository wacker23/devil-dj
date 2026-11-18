#pragma once

// Second-order IIR sections (RBJ "Audio EQ Cookbook" designs), transposed direct form II, double precision.
// Header-only; no allocation, safe on the audio thread.

#include <cmath>
#include <numbers>

namespace devildj::dsp
{

struct BiquadCoeffs
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; // normalised (a0 = 1)

    static BiquadCoeffs normalised(double b0, double b1, double b2, double a0, double a1, double a2) noexcept
    {
        return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
    }

    struct Prewarp
    {
        double cosW, alpha;
    };
    static Prewarp prewarp(double hz, double sampleRate, double q) noexcept
    {
        const double w = 2.0 * std::numbers::pi * hz / sampleRate;
        return {std::cos(w), std::sin(w) / (2.0 * q)};
    }

    static BiquadCoeffs lowPass(double hz, double sampleRate, double q) noexcept
    {
        const auto [c, a] = prewarp(hz, sampleRate, q);
        return normalised((1.0 - c) / 2.0, 1.0 - c, (1.0 - c) / 2.0, 1.0 + a, -2.0 * c, 1.0 - a);
    }

    static BiquadCoeffs highPass(double hz, double sampleRate, double q) noexcept
    {
        const auto [c, a] = prewarp(hz, sampleRate, q);
        return normalised((1.0 + c) / 2.0, -(1.0 + c), (1.0 + c) / 2.0, 1.0 + a, -2.0 * c, 1.0 - a);
    }

    static BiquadCoeffs allPass(double hz, double sampleRate, double q) noexcept
    {
        const auto [c, a] = prewarp(hz, sampleRate, q);
        return normalised(1.0 - a, -2.0 * c, 1.0 + a, 1.0 + a, -2.0 * c, 1.0 - a);
    }

    static BiquadCoeffs peak(double hz, double sampleRate, double q, double gainDb) noexcept
    {
        const auto [c, a] = prewarp(hz, sampleRate, q);
        const double A = std::pow(10.0, gainDb / 40.0);
        return normalised(1.0 + a * A, -2.0 * c, 1.0 - a * A, 1.0 + a / A, -2.0 * c, 1.0 - a / A);
    }

    static BiquadCoeffs lowShelf(double hz, double sampleRate, double q, double gainDb) noexcept
    {
        const auto [c, a] = prewarp(hz, sampleRate, q);
        const double A = std::pow(10.0, gainDb / 40.0);
        const double s = 2.0 * std::sqrt(A) * a;
        return normalised(A * ((A + 1.0) - (A - 1.0) * c + s), 2.0 * A * ((A - 1.0) - (A + 1.0) * c),
                          A * ((A + 1.0) - (A - 1.0) * c - s), (A + 1.0) + (A - 1.0) * c + s,
                          -2.0 * ((A - 1.0) + (A + 1.0) * c), (A + 1.0) + (A - 1.0) * c - s);
    }

    static BiquadCoeffs highShelf(double hz, double sampleRate, double q, double gainDb) noexcept
    {
        const auto [c, a] = prewarp(hz, sampleRate, q);
        const double A = std::pow(10.0, gainDb / 40.0);
        const double s = 2.0 * std::sqrt(A) * a;
        return normalised(A * ((A + 1.0) + (A - 1.0) * c + s), -2.0 * A * ((A - 1.0) + (A + 1.0) * c),
                          A * ((A + 1.0) + (A - 1.0) * c - s), (A + 1.0) - (A - 1.0) * c + s,
                          2.0 * ((A - 1.0) - (A + 1.0) * c), (A + 1.0) - (A - 1.0) * c - s);
    }
};

// One channel of state.
struct BiquadState
{
    double z1 = 0.0, z2 = 0.0;

    double process(const BiquadCoeffs& k, double x) noexcept
    {
        const double y = k.b0 * x + z1;
        z1 = k.b1 * x - k.a1 * y + z2;
        z2 = k.b2 * x - k.a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0.0; }
};

// Butterworth 4th order = two sections with these Qs; Linkwitz-Riley 8th order = Butterworth 4th squared.
inline constexpr double kButterworth4Q[2] = {0.54119610014619698, 1.3065629648763766};

} // namespace devildj::dsp
