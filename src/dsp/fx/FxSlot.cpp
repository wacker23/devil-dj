#include "dsp/fx/FxSlot.h"

#include <algorithm>
#include <cmath>

namespace devildj::dsp::fx
{

void FxSlot::prepare(double sampleRate, int maxBlockSize)
{
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    send.prepare(rate, kFadeSeconds);
    wetRamp.prepare(rate, kFadeSeconds);
    silentLimit = std::max(1, static_cast<int>(std::lround(kSilentSeconds * rate)));
    tailLimit = std::max(1, static_cast<int>(std::lround(kMaxTailSeconds * rate)));
    const auto n = static_cast<std::size_t>(std::max(1, maxBlockSize));
    wetL.assign(n, 0.0f);
    wetR.assign(n, 0.0f);
    reset();
}

void FxSlot::reset() noexcept
{
    current = Stage::Off;
    send.snap(0.0);
    wetRamp.snap(wetTarget);
    silentSamples = tailSamples = 0;
    if (fx != nullptr)
        fx->reset();
    if (requestedOn && fx != nullptr) // still switched on: start again
    {
        current = Stage::On;
        send.setTarget(1.0);
    }
}

void FxSlot::setEffect(Effect* effect) noexcept
{
    fx = effect;
    current = Stage::Off;
    send.snap(0.0);
    silentSamples = tailSamples = 0;
    if (fx != nullptr)
    {
        fx->reset();
        if (requestedOn)
        {
            current = Stage::On;
            send.setTarget(1.0);
        }
    }
}

void FxSlot::setOn(bool on) noexcept
{
    requestedOn = on;
    if (fx == nullptr)
        return;
    if (on)
    {
        if (current == Stage::Off)
        {
            fx->reset();
            send.snap(0.0);
            wetRamp.snap(wetTarget);
        }
        current = Stage::On;
        send.setTarget(1.0);
    }
    else if (current == Stage::On)
    {
        current = Stage::FadingOut;
        send.setTarget(0.0);
    }
}

void FxSlot::setWet(float wet) noexcept
{
    wetTarget = std::clamp(wet, 0.0f, 1.0f);
    if (current == Stage::Off)
        wetRamp.snap(wetTarget);
    else
        wetRamp.setTarget(wetTarget);
}

void FxSlot::process(float* left, float* right, int numSamples, const BeatContext& beat) noexcept
{
    if (current == Stage::Off || fx == nullptr)
        return; // bypass: not a sample touched
    const bool tail = fx->hasTail();
    for (int done = 0; done < numSamples;)
    {
        const int n = std::min(numSamples - done, static_cast<int>(wetL.size()));
        float* l = left + done;
        float* r = right + done;
        // The send into the effect (a copy of the ramp: the mix below steps the real one over the same samples).
        if (current == Stage::Tail)
        {
            std::fill(wetL.begin(), wetL.begin() + n, 0.0f);
            std::fill(wetR.begin(), wetR.begin() + n, 0.0f);
        }
        else
        {
            LinearRamp sendCopy = send;
            for (int i = 0; i < n; ++i)
            {
                const auto m = static_cast<float>(sendCopy.next());
                wetL[static_cast<std::size_t>(i)] = l[i] * m;
                wetR[static_cast<std::size_t>(i)] = r[i] * m;
            }
        }
        fx->process(wetL.data(), wetR.data(), n, beat);

        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const double m = current == Stage::Tail ? 0.0 : send.next();
            const double w = wetRamp.next();
            const auto el = wetL[static_cast<std::size_t>(i)];
            const auto er = wetR[static_cast<std::size_t>(i)];
            const auto dry = static_cast<float>(1.0 - w * m);
            // With a tail the wet path is never faded: the effect starts from silence and its tail is the fade.
            const auto g = static_cast<float>(tail ? w : w * m);
            l[i] = dry * l[i] + g * el;
            r[i] = dry * r[i] + g * er;
            peak = std::max(peak, std::max(std::abs(el), std::abs(er)));
        }

        if (current == Stage::FadingOut && !send.isRamping())
        {
            if (tail)
            {
                current = Stage::Tail;
                silentSamples = tailSamples = 0;
            }
            else
            {
                current = Stage::Off;
                fx->reset();
            }
        }
        else if (current == Stage::Tail)
        {
            silentSamples = peak < kSilence ? silentSamples + n : 0;
            tailSamples += n;
            if (silentSamples >= silentLimit || tailSamples >= tailLimit)
            {
                current = Stage::Off;
                fx->reset();
            }
        }
        done += n;
    }
}

} // namespace devildj::dsp::fx
