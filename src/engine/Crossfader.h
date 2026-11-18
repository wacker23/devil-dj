#pragma once

// Crossfader curves and channel assignment (ARCHITECTURE §4). Position 0 = fully A side, 1 = fully B side.
//   Smooth (constant power): gA = cos(x pi/2), gB = sin(x pi/2)  -> both -3 dB in the middle
//   Linear:                  gA = 1 - x,       gB = x             -> both -6 dB in the middle
//   Cut (scratch):           full level until 5 % from the far end, then a straight line to silence
// A channel assigned THRU ignores the crossfader.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace devildj::engine
{

enum class CrossfaderCurve : std::uint8_t
{
    Smooth,
    Linear,
    Cut,
};

enum class CrossfaderAssign : std::uint8_t
{
    A,
    Thru,
    B,
};

constexpr double kCrossfaderCutZone = 0.05;

// Gain of the A side at position x (the B side is the mirror image: crossfaderGainA(1 - x)).
inline double crossfaderGainA(double x, CrossfaderCurve curve) noexcept
{
    x = std::clamp(x, 0.0, 1.0);
    switch (curve)
    {
        case CrossfaderCurve::Linear:
            return 1.0 - x;
        case CrossfaderCurve::Cut:
            return x <= 1.0 - kCrossfaderCutZone ? 1.0 : (1.0 - x) / kCrossfaderCutZone;
        case CrossfaderCurve::Smooth:
        default:
            return x >= 1.0 ? 0.0 : std::cos(x * std::numbers::pi / 2.0);
    }
}

inline double crossfaderGain(CrossfaderAssign assign, double x, CrossfaderCurve curve) noexcept
{
    switch (assign)
    {
        case CrossfaderAssign::A:
            return crossfaderGainA(x, curve);
        case CrossfaderAssign::B:
            return crossfaderGainA(1.0 - x, curve);
        case CrossfaderAssign::Thru:
        default:
            return 1.0;
    }
}

} // namespace devildj::engine
