#include "dsp/fx/ColorFx.h"

#include "dsp/fx/BeatFx.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace devildj::dsp::fx
{

namespace
{
constexpr float kSilence = 1.0e-5f;
constexpr double kSilentSeconds = 0.05;

// Leaves everything up to 0 dBFS alone and bends what is above it so it never reaches +6 dBFS (slope 1 at 1).
inline float safety(float x) noexcept
{
    const float a = std::abs(x);
    if (a <= 1.0f)
        return x;
    const float over = a - 1.0f;
    return std::copysign(1.0f + over / (1.0f + over), x);
}
} // namespace

const char* ColorFx::typeName(Type type) noexcept
{
    switch (type)
    {
        case Type::Filter:
            return "Filter";
        case Type::Space:
            return "Space";
        case Type::DubEcho:
            return "Dub Echo";
        case Type::Noise:
            return "Noise";
        case Type::Crush:
            return "Crush";
        case Type::Pitch:
            return "Pitch";
        case Type::Count:
            break;
    }
    return "";
}

ColorFx::ColorFx() = default;
ColorFx::~ColorFx() = default;

double ColorFx::signedAmount(double knob) noexcept
{
    const double x = std::clamp(knob, 0.0, 1.0) - 0.5;
    const double edge = 0.5 - kDeadZone;
    if (std::abs(x) <= kDeadZone)
        return 0.0;
    return (x > 0.0 ? x - kDeadZone : x + kDeadZone) / edge;
}

void ColorFx::prepare(double rate, int maxBlockSize)
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    const int block = std::max(1, maxBlockSize);
    amount.prepare(sampleRate, kSmoothingSeconds);
    outGain.prepare(sampleRate, kSmoothingSeconds);
    if (reverb == nullptr)
    {
        reverb = makeEffect(EffectType::Reverb);
        echo = makeEffect(EffectType::Echo);
        noise = makeEffect(EffectType::Noise);
        crush = makeEffect(EffectType::BitCrush);
        pitch = makeSignalsmithStretcher();
    }
    for (auto* e : {reverb.get(), echo.get(), noise.get(), crush.get()})
        e->prepare(sampleRate, block);
    reverb->setParam(0, 0.75f); // decay
    reverb->setParam(1, 0.3f);  // damping
    reverb->setParam(2, 0.7f);  // size
    echo->setBeats(0.75);
    echo->setParam(0, 0.75f / 0.9f); // feedback 0.75
    pitch->prepare(2, sampleRate);
    pitchPrime.assign(static_cast<std::size_t>(pitch->seekLength()), 0.0f);
    for (auto* v : {&wetL, &wetR, &outL, &outR})
        v->assign(static_cast<std::size_t>(block), 0.0f);
    reset();
}

void ColorFx::reset() noexcept
{
    current = wanted;
    amount.snap(current == Type::Filter ? 0.0 : signedAmount(knobValue));
    outGain.snap(1.0);
    active = current != Type::Filter && amount.value() != 0.0;
    silentSamples = 0;
    clearEffects();
}

void ColorFx::setType(Type type) noexcept
{
    if (type == wanted)
        return;
    wanted = type;
    if (!active) // nothing sounding: change at once
    {
        startType();
        return;
    }
    outGain.setTarget(0.0); // fade the old one out; process() switches when it is silent
}

void ColorFx::clearEffects() noexcept
{
    for (auto* e : {reverb.get(), echo.get(), noise.get(), crush.get()})
        if (e != nullptr)
            e->reset();
    fl.reset();
    fr.reset();
    if (pitch != nullptr)
    {
        pitch->reset();
        const float* in[] = {pitchPrime.data(), pitchPrime.data()};
        pitch->seek(in, static_cast<int>(pitchPrime.size()), 1.0);
        pitchShown = 1000.0;
    }
}

void ColorFx::startType() noexcept
{
    current = wanted;
    clearEffects();
    amount.snap(0.0);
    outGain.snap(1.0);
    silentSamples = 0;
    active = false;
    setKnob(knobValue); // the new character comes in from nothing, over 20 ms
}

void ColorFx::setKnob(double k) noexcept
{
    knobValue = std::clamp(k, 0.0, 1.0);
    if (current == Type::Filter || wanted != current)
        return; // (a type change in progress picks the knob up when it starts)
    const double target = signedAmount(knobValue);
    amount.setTarget(target);
    if (target != 0.0 && !active)
    {
        clearEffects(); // nothing stale from the last time it was on
        active = true;
    }
}

double ColorFx::gFor(double hz) const noexcept
{
    return std::tan(std::numbers::pi * std::clamp(hz, 10.0, 0.45 * sampleRate) / sampleRate);
}

float ColorFx::Filter2::process(float x, double g, bool highPass) noexcept
{
    constexpr double k = 1.2; // a little resonance (Q ~0.83)
    const double a1 = 1.0 / (1.0 + g * (g + k));
    const double a2 = g * a1;
    const double a3 = g * a2;
    const double v3 = x - ic2;
    const double v1 = a1 * ic1 + a2 * v3;
    const double v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0 * v1 - ic1;
    ic2 = 2.0 * v2 - ic2;
    return static_cast<float>(highPass ? x - k * v1 - v2 : v2);
}

bool ColorFx::ringing(const float* l, const float* r, int n) noexcept
{
    float peak = 0.0f;
    for (int i = 0; i < n; ++i)
        peak = std::max({peak, std::abs(l[i]), std::abs(r[i])});
    silentSamples = peak < kSilence ? silentSamples + n : 0;
    return silentSamples < static_cast<int>(kSilentSeconds * sampleRate);
}

void ColorFx::process(float* left, float* right, int numSamples, const BeatContext& beat) noexcept
{
    if (!active || current == Type::Filter)
    {
        if (wanted != current && !active)
            startType();
        return; // off: untouched
    }
    for (int done = 0; done < numSamples;)
    {
        const int n = std::min(numSamples - done, static_cast<int>(wetL.size()));
        float* l = left + done;
        float* r = right + done;
        // The amount for this sub-block (its ramp is stepped per sample below; effects get the mean).
        const double startAmount = amount.value();
        switch (current)
        {
            case Type::Space:
            case Type::DubEcho:
            {
                Effect& fx = current == Type::Space ? *reverb : *echo;
                LinearRamp a = amount;
                for (int i = 0; i < n; ++i)
                {
                    const auto send = static_cast<float>(std::abs(a.next()));
                    wetL[static_cast<std::size_t>(i)] = l[i] * send;
                    wetR[static_cast<std::size_t>(i)] = r[i] * send;
                }
                if (current == Type::DubEcho)
                    fx.setParam(1, startAmount < 0.0 ? 0.25f : 1.0f); // tone: dark / bright
                fx.process(wetL.data(), wetR.data(), n, beat);
                const bool high = startAmount > 0.0 || (startAmount == 0.0 && amount.value() >= 0.0);
                const double g = gFor(high ? 700.0 : 1800.0);
                for (int i = 0; i < n; ++i)
                {
                    amount.next();
                    const auto gain = static_cast<float>(outGain.next());
                    const float wl = fl.process(wetL[static_cast<std::size_t>(i)], g, high);
                    const float wr = fr.process(wetR[static_cast<std::size_t>(i)], g, high);
                    wetL[static_cast<std::size_t>(i)] = wl;
                    wetR[static_cast<std::size_t>(i)] = wr;
                    l[i] = safety(l[i] + 0.8f * gain * wl);
                    r[i] = safety(r[i] + 0.8f * gain * wr);
                }
                // At the centre the effect rings out; once it is silent, off.
                if (amount.value() == 0.0 && !amount.isRamping() && !ringing(wetL.data(), wetR.data(), n))
                    active = false;
                break;
            }
            case Type::Noise:
            {
                const double a = std::abs(startAmount);
                noise->setParam(0, static_cast<float>(std::min(1.0, a)));
                noise->setParam(1, 0.6f);
                for (int i = 0; i < n; ++i)
                {
                    wetL[static_cast<std::size_t>(i)] = l[i];
                    wetR[static_cast<std::size_t>(i)] = r[i];
                }
                noise->process(wetL.data(), wetR.data(), n, beat);
                for (int i = 0; i < n; ++i)
                {
                    const double s = amount.next();
                    const auto gain = static_cast<float>(outGain.next());
                    const bool high = s > 0.0;
                    const double hz = high ? 20.0 * std::pow(150.0, std::abs(s)) : 20000.0 * std::pow(1.0 / 40.0, std::abs(s));
                    const double g = gFor(hz);
                    const float wl = fl.process(wetL[static_cast<std::size_t>(i)], g, high);
                    const float wr = fr.process(wetR[static_cast<std::size_t>(i)], g, high);
                    l[i] = safety((1.0f - gain) * l[i] + gain * wl);
                    r[i] = safety((1.0f - gain) * r[i] + gain * wr);
                }
                if (amount.value() == 0.0 && !amount.isRamping())
                    active = false;
                break;
            }
            case Type::Crush:
            {
                const double a = std::abs(startAmount);
                crush->setParam(0, startAmount < 0.0 ? static_cast<float>(a) : 0.0f); // left: bits
                crush->setParam(1, startAmount > 0.0 ? static_cast<float>(a) : 0.0f); // right: sample rate
                for (int i = 0; i < n; ++i)
                {
                    wetL[static_cast<std::size_t>(i)] = l[i];
                    wetR[static_cast<std::size_t>(i)] = r[i];
                }
                crush->process(wetL.data(), wetR.data(), n, beat);
                for (int i = 0; i < n; ++i)
                {
                    const auto m = static_cast<float>(std::min(1.0, 3.0 * std::abs(amount.next())) * outGain.next());
                    l[i] = (1.0f - m) * l[i] + m * wetL[static_cast<std::size_t>(i)];
                    r[i] = (1.0f - m) * r[i] + m * wetR[static_cast<std::size_t>(i)];
                }
                if (amount.value() == 0.0 && !amount.isRamping())
                    active = false;
                break;
            }
            case Type::Pitch:
            {
                const double semitones = 12.0 * startAmount;
                if (std::abs(semitones - pitchShown) > 1e-3)
                {
                    pitch->setTransposeSemitones(semitones);
                    pitchShown = semitones;
                }
                const float* in[] = {l, r};
                float* out[] = {outL.data(), outR.data()};
                pitch->process(in, n, out, n);
                for (int i = 0; i < n; ++i)
                {
                    const auto m = static_cast<float>(std::min(1.0, 5.0 * std::abs(amount.next())) * outGain.next());
                    l[i] = safety((1.0f - m) * l[i] + m * outL[static_cast<std::size_t>(i)]);
                    r[i] = safety((1.0f - m) * r[i] + m * outR[static_cast<std::size_t>(i)]);
                }
                if (amount.value() == 0.0 && !amount.isRamping())
                    active = false;
                break;
            }
            case Type::Filter:
            case Type::Count:
                break;
        }
        done += n;
        if (wanted != current && !outGain.isRamping()) // faded out: the new character
        {
            startType();
            break;
        }
        if (!active)
            break;
    }
}

} // namespace devildj::dsp::fx
