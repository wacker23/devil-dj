#include "Limiter.h"

#include <algorithm>
#include <cmath>

namespace devildj::dsp
{

void Limiter::prepare(double sampleRate, int /*maxBlockSize*/)
{
    window = std::max(2, static_cast<int>(std::lround(kLookaheadSeconds * sampleRate)));
    ceilingGain = static_cast<float>(std::pow(10.0, kCeilingDb / 20.0));
    releaseCoeff = 1.0 - std::exp(-1.0 / (kReleaseSeconds * sampleRate));
    delayL.assign(static_cast<std::size_t>(window), 0.0f);
    delayR.assign(static_cast<std::size_t>(window), 0.0f);
    held.assign(static_cast<std::size_t>(window), 1.0);
    required.assign(static_cast<std::size_t>(window) + 1, 1.0);
    reset();
}

void Limiter::reset() noexcept
{
    std::fill(delayL.begin(), delayL.end(), 0.0f);
    std::fill(delayR.begin(), delayR.end(), 0.0f);
    std::fill(held.begin(), held.end(), 1.0);
    std::fill(required.begin(), required.end(), 1.0);
    pos = 0;
    requiredPos = 0;
    envelope = 1.0;
    lastReductionDb = 0.0f;
}

void Limiter::process(float* left, float* right, int numSamples) noexcept
{
    const auto w = static_cast<std::size_t>(window);
    const double ceilingD = ceilingGain;
    double minGain = 1.0;
    for (int i = 0; i < numSamples; ++i)
    {
        const auto p = static_cast<std::size_t>(pos);
        const double peak = std::max(std::abs(static_cast<double>(left[i])), std::abs(static_cast<double>(right[i])));
        required[static_cast<std::size_t>(requiredPos)] = peak > ceilingD ? ceilingD / peak : 1.0;
        requiredPos = requiredPos == window ? 0 : requiredPos + 1;

        // Min over the last window + 1 requirements: every held value averaged below covers the sample that
        // leaves the delay line (window samples old) as well as the newest one.
        double hold = 1.0;
        for (double r : required)
            hold = std::min(hold, r);
        envelope = std::min(hold, envelope + (1.0 - envelope) * releaseCoeff);
        held[p] = envelope;

        double sum = 0.0;
        for (std::size_t k = 0; k < w; ++k)
            sum += held[k];
        const double gain = std::min(1.0, sum / static_cast<double>(window));
        minGain = std::min(minGain, gain);

        // The sample leaving the delay line is the oldest one: the slot about to be overwritten.
        const float outL = delayL[p], outR = delayR[p];
        delayL[p] = left[i];
        delayR[p] = right[i];
        left[i] = std::clamp(static_cast<float>(outL * gain), -ceilingGain, ceilingGain);
        right[i] = std::clamp(static_cast<float>(outR * gain), -ceilingGain, ceilingGain);

        pos = pos + 1 == window ? 0 : pos + 1;
    }
    lastReductionDb = static_cast<float>(-20.0 * std::log10(std::max(minGain, 1e-6)));
}

} // namespace devildj::dsp
