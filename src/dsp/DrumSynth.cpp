#include "dsp/DrumSynth.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace devildj::dsp
{

namespace
{
constexpr const char* kNames[kDrumKinds] = {"kick",   "snare",  "clap", "closedHat", "openHat", "tomLow",
                                            "tomMid", "tomHigh", "rim", "cowbell",   "shaker"};
// Hats: six square oscillators at inharmonic ratios of the base.
constexpr double kHatRatios[6] = {2.0, 3.0, 4.16, 5.43, 6.79, 8.21};
constexpr double kTwoPi = 2.0 * std::numbers::pi;

float clamp01(float v)
{
    return std::clamp(v, 0.0f, 1.0f);
}
float square(double phase)
{
    return phase < 0.5 ? 1.0f : -1.0f;
}
float triangle(double phase)
{
    return static_cast<float>(4.0 * std::abs(phase - 0.5) - 1.0);
}
} // namespace

DrumKind drumKindFromName(const char* name) noexcept
{
    for (int i = 0; i < kDrumKinds; ++i)
        if (name != nullptr && std::strcmp(name, kNames[i]) == 0)
            return static_cast<DrumKind>(i);
    return DrumKind::Kick;
}

const char* drumKindName(DrumKind kind) noexcept
{
    return kNames[std::clamp(static_cast<int>(kind), 0, kDrumKinds - 1)];
}

void DrumVoice::setDecay(int which, double seconds) noexcept
{
    decayK[which] = static_cast<float>(std::exp(-6.907755 / (std::max(0.001, seconds) * rate)));
}

float DrumVoice::noise() noexcept
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return static_cast<float>(static_cast<double>(rng) / 2147483648.0 - 1.0);
}

float DrumVoice::band(int which, float x) noexcept
{
    return static_cast<float>(state[which].process(coeff[which], x).band * coeff[which].k);
}

float DrumVoice::high(int which, float x) noexcept
{
    return static_cast<float>(state[which].process(coeff[which], x).high);
}

void DrumVoice::trigger(DrumKind kind, const DrumParams& params, float vel, std::uint32_t seed) noexcept
{
    what = kind;
    p = params;
    for (auto* v : {&p.tune, &p.decay, &p.tone, &p.snap, &p.drive, &p.pitchAmount, &p.click, &p.spread})
        *v = clamp01(*v);
    velocity = clamp01(vel);
    rng = seed * 2654435761u + 0x9e3779b9u;
    if (rng == 0)
        rng = 1;
    t = 0;
    done = false;
    for (int i = 0; i < 6; ++i)
        phase[i] = hz[i] = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        env[i] = 0.0f;
        decayK[i] = 0.0f;
    }
    for (auto& s : state)
        s.reset();
    pitchDrop = 0.0f;
    spacing = 0;
    const double r = rate;
    switch (kind)
    {
        case DrumKind::Kick:
            hz[0] = 35.0 + 45.0 * p.tune;
            pitchDrop = 3.0f + 4.0f * p.pitchAmount; // starts 4-8x the base
            env[0] = 1.0f;
            setDecay(0, 0.1 + 1.4 * p.decay);
            env[1] = p.click;
            setDecay(1, 0.004);
            env[2] = 1.0f;
            decayK[2] = static_cast<float>(std::exp(-1.0 / ((0.02 + 0.06 * p.pitchAmount) / 3.0 * r))); // 20-80 ms
            coeff[0] = SvfCoeffs::make(1500.0, r, 0.7071);
            break;
        case DrumKind::Snare:
            hz[0] = 150.0 + 100.0 * p.tune;
            hz[1] = hz[0] * 1.6;
            env[0] = 1.0f;
            setDecay(0, 0.06 + 0.16 * p.decay);
            env[1] = 1.0f;
            setDecay(1, 0.08 + 0.25 * p.snap);
            coeff[0] = SvfCoeffs::make(4000.0 + 2000.0 * p.tone, r, 0.8);
            break;
        case DrumKind::Clap:
            coeff[0] = SvfCoeffs::make(1000.0 + 500.0 * p.tone, r, 2.5);
            spacing = static_cast<std::int64_t>((0.008 + 0.004 * p.spread) * r);
            env[0] = 1.0f;
            setDecay(0, 0.012);
            setDecay(1, 0.1 + 0.5 * p.decay);
            break;
        case DrumKind::ClosedHat:
        case DrumKind::OpenHat:
            for (int i = 0; i < 6; ++i)
                hz[i] = (300.0 + 250.0 * p.tune) * kHatRatios[i];
            env[0] = 1.0f;
            setDecay(0, kind == DrumKind::ClosedHat ? 0.02 + 0.1 * p.decay : 0.2 + 0.8 * p.decay);
            coeff[0] = SvfCoeffs::make(7000.0 + 3000.0 * p.tone, r, 1.0);
            coeff[1] = SvfCoeffs::make(7000.0, r, 0.7071);
            break;
        case DrumKind::TomLow:
        case DrumKind::TomMid:
        case DrumKind::TomHigh:
        {
            const double lo = kind == DrumKind::TomLow ? 80.0 : kind == DrumKind::TomMid ? 120.0 : 180.0;
            const double span = kind == DrumKind::TomLow ? 40.0 : kind == DrumKind::TomMid ? 60.0 : 80.0;
            hz[0] = lo + span * p.tune;
            pitchDrop = 0.5f * p.pitchAmount;
            env[0] = 1.0f;
            setDecay(0, 0.15 + 0.65 * p.decay);
            env[1] = 0.05f;
            setDecay(1, 0.02);
            env[2] = 1.0f;
            decayK[2] = static_cast<float>(std::exp(-1.0 / (0.05 * r)));
            break;
        }
        case DrumKind::Rim:
            hz[0] = 1500.0 + 1000.0 * p.tune;
            env[0] = 1.0f;
            setDecay(0, 0.01 + 0.03 * p.decay);
            env[1] = 1.0f;
            setDecay(1, 0.005);
            coeff[0] = SvfCoeffs::make(3000.0, r, 3.0);
            break;
        case DrumKind::Cowbell:
        {
            const double f = 0.7 + 0.7 * p.tune;
            hz[0] = 540.0 * f;
            hz[1] = 800.0 * f;
            env[0] = 0.7f;
            setDecay(0, 0.03);
            env[1] = 0.3f;
            setDecay(1, 0.2 + 0.5 * p.decay);
            coeff[0] = SvfCoeffs::make(1200.0 * f, r, 1.5);
            break;
        }
        case DrumKind::Shaker:
            env[0] = 1.0f;
            setDecay(0, 0.05 + 0.25 * p.decay);
            coeff[0] = SvfCoeffs::make(5000.0 + 4000.0 * p.tone, r, 0.7071);
            break;
    }
}

float DrumVoice::next() noexcept
{
    if (done)
        return 0.0f;
    const double r = rate;
    float x = 0.0f;
    const auto advance = [&](int i, double f)
    {
        phase[i] += f / r;
        phase[i] -= std::floor(phase[i]);
    };
    switch (what)
    {
        case DrumKind::Kick:
        {
            advance(0, hz[0] * (1.0 + pitchDrop * env[2]));
            const float body = static_cast<float>(std::sin(kTwoPi * phase[0])) * env[0];
            const float click = high(0, noise()) * env[1];
            x = body + 0.5f * click;
            break;
        }
        case DrumKind::Snare:
        {
            advance(0, hz[0]);
            advance(1, hz[1]);
            const float body = 0.5f * (triangle(phase[0]) + triangle(phase[1])) * env[0];
            const float n = band(0, noise()) * env[1];
            x = (1.0f - 0.7f * p.tone) * body + (0.4f + 0.8f * p.tone) * n;
            break;
        }
        case DrumKind::Clap:
        {
            if (spacing > 0 && t > 0 && t <= 3 * spacing && t % spacing == 0)
                env[0] = 1.0f; // the next burst
            if (t == 3 * spacing)
                env[1] = 0.6f; // the tail
            x = 2.0f * band(0, noise()) * (env[0] + env[1]);
            break;
        }
        case DrumKind::ClosedHat:
        case DrumKind::OpenHat:
        {
            float metal = 0.0f;
            for (int i = 0; i < 6; ++i)
            {
                advance(i, hz[i]);
                metal += square(phase[i]);
            }
            x = 0.6f * high(1, band(0, metal / 6.0f)) * env[0];
            break;
        }
        case DrumKind::TomLow:
        case DrumKind::TomMid:
        case DrumKind::TomHigh:
            advance(0, hz[0] * (1.0 + pitchDrop * env[2]));
            x = static_cast<float>(std::sin(kTwoPi * phase[0])) * env[0] + noise() * env[1];
            break;
        case DrumKind::Rim:
            advance(0, hz[0]);
            x = 0.6f * static_cast<float>(std::sin(kTwoPi * phase[0])) * env[0] + 0.8f * band(0, noise()) * env[1];
            break;
        case DrumKind::Cowbell:
            advance(0, hz[0]);
            advance(1, hz[1]);
            x = 0.8f * band(0, 0.5f * (square(phase[0]) + square(phase[1]))) * (env[0] + env[1]);
            break;
        case DrumKind::Shaker:
        {
            const float attack = std::min(1.0f, static_cast<float>(t) / static_cast<float>(0.01 * r));
            x = 0.8f * high(0, noise()) * env[0] * attack;
            break;
        }
    }
    for (int i = 0; i < 3; ++i)
        env[i] *= decayK[i];
    ++t;
    x *= velocity;
    if (p.drive > 0.0f)
    {
        const float g = 1.0f + 9.0f * p.drive;
        x = std::tanh(g * x) / std::tanh(g);
    }
    // Finished once its envelopes are below -80 dB (a clap only after its last burst).
    if (env[0] + env[1] < 1e-4f && (what != DrumKind::Clap || t > 3 * spacing))
        done = true;
    return x;
}

} // namespace devildj::dsp
