#pragma once

// The Beat Maker's synth engines (BEATMAKER_SPEC §5), one note per SynthVoice:
// - Subtractive: 2 PolyBLEP oscillators (saw / square / triangle / sine, detune, octave) + a sub (-1 octave square
//   or sine) + noise -> a TPT state-variable filter (LP / BP / HP, 12 or 24 dB, cutoff, resonance, envelope amount,
//   key tracking) -> amp; ADSRs for filter and amp; glide; unison of up to 8 detuned copies ("supersaw").
// - FM: 2 operators (carrier + modulator at a ratio), the index with its own envelope, modulator feedback.
// - Sub-bass: a sine + an optional 2nd harmonic, drive, glide, a short attack click.
// Every parameter is a float (choices as whole numbers) so a preset is a list of named numbers. Deterministic
// (seeded per note), no allocation, trivially copyable.

#include "dsp/StateVariableFilter.h"

#include <cstdint>

namespace devildj::dsp
{

enum class SynthEngine : std::uint8_t
{
    Subtractive,
    Fm,
    SubBass,
};

struct Adsr
{
    float attack = 0.005f, decay = 0.2f, sustain = 0.7f, release = 0.2f; // seconds, level 0..1
};

struct SynthParams
{
    SynthEngine engine = SynthEngine::Subtractive;
    float gain = 0.5f;
    float glide = 0.0f; // seconds from the previous note's pitch
    Adsr amp;
    // Subtractive
    float osc1Wave = 0, osc2Wave = 0; // 0 saw, 1 square, 2 triangle, 3 sine
    float osc2Detune = 7.0f;          // cents
    float osc2Octave = 0.0f;          // -2 .. +2
    float oscMix = 0.5f;              // 0 = osc 1 only, 1 = osc 2 only
    float sub = 0.0f, subWave = 0.0f; // level; 0 square, 1 sine
    float noise = 0.0f;
    float filterMode = 0.0f; // 0 LP, 1 BP, 2 HP
    float filter24 = 0.0f;   // 1 = 24 dB / octave
    float cutoff = 2000.0f;  // Hz
    float resonance = 0.2f;  // 0..1
    float envAmount = 0.5f;  // -1..1: up to +-6 octaves by the filter envelope
    float keyTrack = 0.5f;   // 0..1
    Adsr filterEnv{0.002f, 0.3f, 0.2f, 0.3f};
    float unison = 1.0f;       // 1..8 copies
    float unisonDetune = 15.0f; // cents across them
    // FM
    float ratio = 2.0f;  // modulator / carrier
    float index = 2.0f;  // peak modulation index
    Adsr indexEnv{0.001f, 0.4f, 0.3f, 0.3f};
    float feedback = 0.0f; // 0..1
    // Sub-bass
    float harmonic2 = 0.0f; // 0..1
    float drive = 0.0f;     // 0..1
    float click = 0.0f;     // 0..1
};

// An envelope: linear attack, exponential decay to the sustain level and release.
class Envelope
{
public:
    void start(const Adsr& a, double sampleRate) noexcept;
    void release() noexcept;
    float next() noexcept;
    bool idle() const noexcept { return stage == Stage::Idle; }

private:
    enum class Stage : std::uint8_t { Idle, Attack, Decay, Sustain, Release };
    Stage stage = Stage::Idle;
    float level = 0.0f, attackStep = 1.0f, decayK = 0.0f, releaseK = 0.0f, sustain = 0.0f;
};

class SynthVoice
{
public:
    static constexpr int kMaxUnison = 8;

    void prepare(double sampleRate) noexcept { rate = sampleRate; }
    // A note (MIDI pitch, may be fractional); glideFrom < 0: no glide.
    void noteOn(const SynthParams& params, float note, float velocity, std::uint32_t seed, float glideFrom = -1.0f) noexcept;
    void noteOff() noexcept;
    float next() noexcept;
    bool finished() const noexcept { return amp.idle(); }

private:
    float noise() noexcept;
    float osc(int wave, double& phase, double inc) noexcept;
    void updateFilter(float filterEnvLevel) noexcept;

    double rate = 48000.0;
    SynthParams p;
    float velocity = 1.0f, note = 60.0f, pitch = 60.0f, glideStep = 0.0f;
    std::uint32_t rng = 1;
    std::int64_t t = 0;
    Envelope amp, filter, index;
    double phase1[kMaxUnison]{}, phase2[kMaxUnison]{}, subPhase = 0.0, carrier = 0.0, modulator = 0.0;
    double spread[kMaxUnison]{}; // frequency ratio per unison copy
    double osc2Ratio = 1.0, hz = 261.6;
    int copies = 1;
    float lastMod = 0.0f;
    SvfCoeffs coeff;
    SvfState state[2];
    int controlCounter = 0;
    float filterLevel = 0.0f;
};

} // namespace devildj::dsp
