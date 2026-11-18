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
    queueValue.assign(static_cast<std::size_t>(window) + 1, 1.0);
    queueIndex.assign(static_cast<std::size_t>(window) + 1, 0);
    reset();
}

void Limiter::reset() noexcept
{
    std::fill(delayL.begin(), delayL.end(), 0.0f);
    std::fill(delayR.begin(), delayR.end(), 0.0f);
    std::fill(held.begin(), held.end(), 1.0);
    heldSum = static_cast<double>(window);
    queueHead = queueSize = 0;
    counter = 0;
    pos = 0;
    envelope = 1.0;
    lastReductionDb = 0.0f;
}

void Limiter::process(float* left, float* right, int numSamples) noexcept
{
    const double ceilingD = ceilingGain;
    const int capacity = window + 1;
    double minGain = 1.0;
    for (int i = 0; i < numSamples; ++i)
    {
        const auto p = static_cast<std::size_t>(pos);
        const double peak = std::max(std::abs(static_cast<double>(left[i])), std::abs(static_cast<double>(right[i])));
        const double need = peak > ceilingD ? ceilingD / peak : 1.0;

        // Running minimum of the last window + 1 requirements (monotonic queue: O(1) per sample). Every held
        // value averaged below then covers the sample leaving the delay line (window samples old) as well as
        // the newest one.
        while (queueSize > 0 && queueValue[static_cast<std::size_t>((queueHead + queueSize - 1) % capacity)] >= need)
            --queueSize;
        const auto back = static_cast<std::size_t>((queueHead + queueSize) % capacity);
        queueValue[back] = need;
        queueIndex[back] = counter;
        ++queueSize;
        while (queueIndex[static_cast<std::size_t>(queueHead)] < counter - window)
        {
            queueHead = (queueHead + 1) % capacity;
            --queueSize;
        }
        const double hold = queueValue[static_cast<std::size_t>(queueHead)];
        ++counter;

        envelope = std::min(hold, envelope + (1.0 - envelope) * releaseCoeff);
        heldSum += envelope - held[p];
        held[p] = envelope;
        if (pos == 0) // re-add exactly once per window so rounding can't drift
        {
            heldSum = 0.0;
            for (double h : held)
                heldSum += h;
        }
        const double gain = std::min(1.0, heldSum / static_cast<double>(window));
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
