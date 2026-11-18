#pragma once

// Parameter smoother: moves linearly to a new target over a fixed time (ARCHITECTURE §4: 20 ms ramps).
// Header-only, no allocation.

#include <algorithm>
#include <cmath>

namespace devildj::dsp
{

class LinearRamp
{
public:
    void prepare(double sampleRate, double rampSeconds) noexcept
    {
        rampSamples = std::max(1, static_cast<int>(std::lround(sampleRate * rampSeconds)));
    }

    void setTarget(double value) noexcept
    {
        if (value == target)
            return;
        target = value;
        remaining = rampSamples;
        step = (target - current) / remaining;
    }

    void snap(double value) noexcept
    {
        current = target = value;
        remaining = 0;
    }

    double next() noexcept
    {
        if (remaining > 0)
            current = --remaining == 0 ? target : current + step;
        return current;
    }

    // Advance several samples at once (for values that are only applied per sub-block).
    double skip(int samples) noexcept
    {
        if (remaining <= 0)
            return current;
        if (samples >= remaining)
        {
            remaining = 0;
            current = target;
        }
        else
        {
            remaining -= samples;
            current += step * samples;
        }
        return current;
    }

    bool isRamping() const noexcept { return remaining > 0; }
    double value() const noexcept { return current; }
    double targetValue() const noexcept { return target; }

private:
    double current = 0.0, target = 0.0, step = 0.0;
    int remaining = 0;
    int rampSamples = 960;
};

} // namespace devildj::dsp
