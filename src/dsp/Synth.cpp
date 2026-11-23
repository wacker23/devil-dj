#include "dsp/Synth.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace devildj::dsp
{

namespace
{
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr int kControlRate = 16; // the filter's cutoff is updated every 16 samples

float factorFor(double seconds, double rate)
{
    // -60 dB after `seconds`
    return static_cast<float>(std::exp(-6.907755 / (std::max(0.001, seconds) * rate)));
}

double hzOf(double midi)
{
    return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
}

// PolyBLEP: the band-limited step's correction near a discontinuity at phase 0.
double blep(double t, double dt)
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt)
    {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}
} // namespace

// ---- Envelope ----

void Envelope::start(const Adsr& a, double rate) noexcept
{
    stage = Stage::Attack;
    level = 0.0f;
    attackStep = static_cast<float>(1.0 / std::max(1.0, std::max(0.0005, static_cast<double>(a.attack)) * rate));
    sustain = std::clamp(a.sustain, 0.0f, 1.0f);
    decayK = factorFor(a.decay, rate);
    releaseK = factorFor(std::max(0.002f, a.release), rate);
}

void Envelope::release() noexcept
{
    if (stage != Stage::Idle)
        stage = Stage::Release;
}

float Envelope::next() noexcept
{
    switch (stage)
    {
        case Stage::Attack:
            level += attackStep;
            if (level >= 1.0f)
            {
                level = 1.0f;
                stage = Stage::Decay;
            }
            break;
        case Stage::Decay:
            level = sustain + (level - sustain) * decayK;
            if (level - sustain < 1e-4f)
            {
                level = sustain;
                stage = Stage::Sustain;
            }
            break;
        case Stage::Sustain:
            break;
        case Stage::Release:
            level *= releaseK;
            if (level < 1e-4f)
            {
                level = 0.0f;
                stage = Stage::Idle;
            }
            break;
        case Stage::Idle:
            return 0.0f;
    }
    if (stage == Stage::Sustain && sustain <= 0.0f)
        stage = Stage::Idle; // a pluck: over once it has decayed
    return level;
}

// ---- SynthVoice ----

float SynthVoice::noise() noexcept
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return static_cast<float>(static_cast<double>(rng) / 2147483648.0 - 1.0);
}

float SynthVoice::osc(int wave, double& phase, double inc) noexcept
{
    phase += inc;
    phase -= std::floor(phase);
    switch (wave)
    {
        case 1: // square
        {
            double v = phase < 0.5 ? 1.0 : -1.0;
            v += blep(phase, inc);
            double shifted = phase + 0.5;
            shifted -= std::floor(shifted);
            v -= blep(shifted, inc);
            return static_cast<float>(v);
        }
        case 2: // triangle (its harmonics fall fast enough without correction)
            return static_cast<float>(4.0 * std::abs(phase - 0.5) - 1.0);
        case 3:
            return static_cast<float>(std::sin(kTwoPi * phase));
        default: // saw
            return static_cast<float>(2.0 * phase - 1.0 - blep(phase, inc));
    }
}

void SynthVoice::noteOn(const SynthParams& params, float n, float vel, std::uint32_t seed, float glideFrom) noexcept
{
    p = params;
    note = std::clamp(n, 0.0f, 127.0f);
    velocity = std::clamp(vel, 0.0f, 1.0f);
    rng = seed * 2654435761u + 0x2545f491u;
    if (rng == 0)
        rng = 1;
    t = 0;
    pitch = note;
    glideStep = 0.0f;
    if (glideFrom >= 0.0f && p.glide > 0.0f)
    {
        pitch = glideFrom;
        glideStep = static_cast<float>((note - glideFrom) / (std::max(0.001, static_cast<double>(p.glide)) * rate));
    }
    copies = p.engine == SynthEngine::Subtractive ? std::clamp(static_cast<int>(std::lround(p.unison)), 1, kMaxUnison) : 1;
    for (int i = 0; i < copies; ++i)
    {
        // Spread evenly across +-detune / 2, starting phases scattered (seeded) so copies don't cancel.
        const double cents = copies == 1 ? 0.0 : (static_cast<double>(i) / (copies - 1) - 0.5) * p.unisonDetune;
        spread[i] = std::pow(2.0, cents / 1200.0);
        phase1[i] = copies == 1 ? 0.0 : 0.5 * (noise() + 1.0f);
        phase2[i] = copies == 1 ? 0.0 : 0.5 * (noise() + 1.0f);
    }
    osc2Ratio = std::pow(2.0, std::clamp(std::round(static_cast<double>(p.osc2Octave)), -2.0, 2.0) +
                                  static_cast<double>(p.osc2Detune) / 1200.0);
    hz = hzOf(pitch);
    subPhase = carrier = modulator = 0.0;
    lastMod = 0.0f;
    for (auto& s : state)
        s.reset();
    controlCounter = 0;
    amp.start(p.amp, rate);
    filter.start(p.filterEnv, rate);
    index.start(p.indexEnv, rate);
    filterLevel = 0.0f;
    updateFilter(0.0f);
}

void SynthVoice::noteOff() noexcept
{
    amp.release();
    filter.release();
    index.release();
}

void SynthVoice::updateFilter(float env) noexcept
{
    const double octaves = std::clamp(static_cast<double>(p.envAmount), -1.0, 1.0) * 6.0 * env +
                           std::clamp(static_cast<double>(p.keyTrack), 0.0, 1.0) * (pitch - 60.0) / 12.0;
    const double cutoffHz = std::clamp(static_cast<double>(p.cutoff) * std::pow(2.0, octaves), 20.0, 0.45 * rate);
    const double q = 0.5 + 12.0 * std::pow(std::clamp(static_cast<double>(p.resonance), 0.0, 1.0), 2.0);
    coeff = SvfCoeffs::make(cutoffHz, rate, q);
}

float SynthVoice::next() noexcept
{
    if (amp.idle())
        return 0.0f;
    // Glide.
    if (glideStep != 0.0f)
    {
        pitch += glideStep;
        if ((glideStep > 0.0f && pitch >= note) || (glideStep < 0.0f && pitch <= note))
        {
            pitch = note;
            glideStep = 0.0f;
        }
        hz = hzOf(pitch);
    }
    const float a = amp.next();
    float x = 0.0f;
    switch (p.engine)
    {
        case SynthEngine::Subtractive:
        {
            const int w1 = std::clamp(static_cast<int>(p.osc1Wave), 0, 3), w2 = std::clamp(static_cast<int>(p.osc2Wave), 0, 3);
            const double hz2 = hz * osc2Ratio;
            float o1 = 0.0f, o2 = 0.0f;
            for (int i = 0; i < copies; ++i)
            {
                o1 += osc(w1, phase1[i], hz * spread[i] / rate);
                o2 += osc(w2, phase2[i], hz2 * spread[i] / rate);
            }
            const float norm = 1.0f / std::sqrt(static_cast<float>(copies));
            const float mix = std::clamp(p.oscMix, 0.0f, 1.0f);
            x = ((1.0f - mix) * o1 + mix * o2) * norm;
            if (p.sub > 0.0f)
                x += std::clamp(p.sub, 0.0f, 1.0f) * osc(p.subWave >= 0.5f ? 3 : 1, subPhase, 0.5 * hz / rate);
            if (p.noise > 0.0f)
                x += std::clamp(p.noise, 0.0f, 1.0f) * noise();
            // The filter (cutoff at control rate), 12 or 24 dB.
            filterLevel = filter.next();
            if (controlCounter-- <= 0)
            {
                updateFilter(filterLevel);
                controlCounter = kControlRate - 1;
            }
            const int mode = std::clamp(static_cast<int>(p.filterMode), 0, 2);
            const auto pick = [&](const SvfState::Out& o) { return mode == 0 ? o.low : mode == 1 ? o.band * coeff.k : o.high; };
            double y = pick(state[0].process(coeff, x));
            if (p.filter24 >= 0.5f)
                y = pick(state[1].process(coeff, y));
            x = static_cast<float>(std::clamp(y, -4.0, 4.0)) * 0.5f;
            break;
        }
        case SynthEngine::Fm:
        {
            const float i = index.next() * std::clamp(p.index, 0.0f, 20.0f);
            modulator += hz * std::clamp(static_cast<double>(p.ratio), 0.125, 16.0) / rate;
            modulator -= std::floor(modulator);
            const float mod = static_cast<float>(std::sin(kTwoPi * modulator + std::clamp(p.feedback, 0.0f, 1.0f) * lastMod * 3.0));
            lastMod = mod;
            carrier += hz / rate;
            carrier -= std::floor(carrier);
            x = static_cast<float>(std::sin(kTwoPi * carrier + i * mod));
            break;
        }
        case SynthEngine::SubBass:
        {
            carrier += hz / rate;
            carrier -= std::floor(carrier);
            x = static_cast<float>(std::sin(kTwoPi * carrier) +
                                   std::clamp(p.harmonic2, 0.0f, 1.0f) * 0.5 * std::sin(2.0 * kTwoPi * carrier));
            if (p.drive > 0.0f)
            {
                const float g = 1.0f + 9.0f * std::clamp(p.drive, 0.0f, 1.0f);
                x = std::tanh(g * x) / std::tanh(g);
            }
            const auto clickLength = static_cast<std::int64_t>(0.003 * rate);
            if (p.click > 0.0f && t < clickLength)
                x += std::clamp(p.click, 0.0f, 1.0f) * 0.5f * noise() * (1.0f - static_cast<float>(t) / static_cast<float>(clickLength));
            x *= 0.8f;
            break;
        }
    }
    ++t;
    return x * a * velocity * std::clamp(p.gain, 0.0f, 2.0f);
}

} // namespace devildj::dsp
