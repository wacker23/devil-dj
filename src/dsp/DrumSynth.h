#pragma once

// The Beat Maker's drum synths (BEATMAKER_SPEC §4): our own recipes, rendered live, one voice at a time.
// Parameters are 0..1 (0.5 = the sound's default) and mapped to each sound's ranges. Every voice ends in a soft
// clipper (`drive`); the per-voice filter is the voice pool's. Deterministic: the noise is seeded per trigger, so
// the same hits render the same samples (offline == real time). No allocation; trivially copyable.

#include "dsp/StateVariableFilter.h"

#include <cstdint>

namespace devildj::dsp
{

enum class DrumKind : std::uint8_t
{
    Kick,
    Snare,
    Clap,
    ClosedHat,
    OpenHat,
    TomLow,
    TomMid,
    TomHigh,
    Rim,
    Cowbell,
    Shaker,
};
constexpr int kDrumKinds = 11;

// Model names ("kick", "closedHat", ...) <-> kinds; unknown names give the kick.
DrumKind drumKindFromName(const char* name) noexcept;
const char* drumKindName(DrumKind kind) noexcept;

struct DrumParams
{
    float tune = 0.5f;        // pitch within the sound's range
    float decay = 0.5f;       // length
    float tone = 0.5f;        // brightness / body-noise mix
    float snap = 0.5f;        // snare: noise decay
    float drive = 0.0f;       // soft clipper
    float pitchAmount = 0.5f; // kick / toms: the pitch drop
    float click = 0.5f;       // kick: the attack click
    float spread = 0.5f;      // clap: time between its bursts
};

class DrumVoice
{
public:
    void prepare(double sampleRate) noexcept { rate = sampleRate; }
    // Start a hit; `seed` makes its noise (the same seed, the same sound).
    void trigger(DrumKind kind, const DrumParams& params, float velocity, std::uint32_t seed) noexcept;
    // The next output sample (mono).
    float next() noexcept;
    bool finished() const noexcept { return done; }
    DrumKind kind() const noexcept { return what; }

private:
    float noise() noexcept;
    float band(int which, float x) noexcept; // unity gain at the centre
    float high(int which, float x) noexcept;
    void setDecay(int which, double seconds) noexcept; // -60 dB after `seconds`

    double rate = 48000.0;
    DrumKind what = DrumKind::Kick;
    DrumParams p;
    float velocity = 1.0f;
    std::uint32_t rng = 1;
    std::int64_t t = 0; // samples since the hit
    bool done = true;
    double phase[6]{};
    double hz[6]{};
    float env[4]{};    // 0, 1: amplitude envelopes; 2: the pitch envelope
    float decayK[4]{}; // their per-sample factors
    SvfCoeffs coeff[2];
    SvfState state[2];
    float pitchDrop = 0.0f; // kick / toms: f = base x (1 + pitchDrop x env[2])
    std::int64_t spacing = 0; // clap: samples between bursts
};

} // namespace devildj::dsp
