#pragma once

// The voices that play scheduled events (ARCHITECTURE §3.7, P4-03): fixed pools of 64 sample voices and 32 synth
// voices shared by the Beat Maker and the code deck. Each voice: an ADSR, a low-pass + high-pass state-variable
// filter, gain and equal-power pan, and sends to its module's delay and reverb buses.
//
// - start(event, offset) begins a voice `offset` samples into the next process() call; process() renders every
//   voice into the buses (added to what is there).
// - Stealing when a pool is full: the oldest releasing voice, else the oldest playing one. The stolen voice is
//   faded out over 2 ms in a spare tail slot so nothing clicks.
// - Choke groups: a new voice in group g (1..255) of a source fades out the other voices of that source and group
//   (5 ms), from the new voice's start.
// - Drum-synth voices (P4-04): 16 per source, each a dsp::DrumVoice for the event's sound (setDrumSound);
//   one-shots (no gate), ended by their own decay, a choke or a steal.
// - Sample voices play a SampleData (resampled linearly to the engine rate, x speed x 2^((note - 60) / 12)) to
//   its end, or until their duration then their release. Synth voices: a sine at the note's pitch until the synth
//   engines arrive (P4-05) - `durationSeconds` is the gate.
//
// Real-time safe: no allocation, no locks; the sample table is set from the message thread only while the
// engine isn't processing (later: through an engine command).

#include "dsp/DrumSynth.h"
#include "dsp/StateVariableFilter.h"
#include "engine/ScheduledEvent.h"

#include <array>
#include <cstdint>
#include <span>

namespace devildj::engine
{

// A sample's audio, owned elsewhere (a SampleBuffer in the registry).
struct SampleData
{
    const float* const* channels = nullptr; // 1 or 2
    int numChannels = 0;
    int numFrames = 0;
    double sampleRate = 48000.0;
};

// Where the voices go: the module's dry mix and its two effect sends (stereo each, numSamples long).
// A drum-synth sound: which drum and its parameters.
struct DrumSound
{
    dsp::DrumKind kind = dsp::DrumKind::Kick;
    dsp::DrumParams params;
};

struct VoiceBuses
{
    float* left = nullptr;
    float* right = nullptr;
    float* delayLeft = nullptr;
    float* delayRight = nullptr;
    float* reverbLeft = nullptr;
    float* reverbRight = nullptr;
};

class Voices
{
public:
    static constexpr int kSampleVoices = 64;
    static constexpr int kSynthVoices = 32;
    static constexpr int kDrumVoicesPerSource = 16;
    static constexpr int kSources = 3;
    static constexpr int kSounds = 256;

    void prepare(double sampleRate, int maxBlockSize) noexcept;
    void reset() noexcept;

    void setSample(std::uint16_t index, const SampleData* data) noexcept;
    void setDrumSound(std::uint16_t index, const DrumSound& sound) noexcept;

    // Audio thread.
    // The voice's id (0 if none was started: a missing sample, a drum synth).
    std::uint64_t start(const ScheduledEvent& e, int offset) noexcept;
    bool playing(std::uint64_t id) const noexcept; // still sounding (not stolen, choked away or finished)
    void process(const VoiceBuses& buses, int numSamples) noexcept;

    int activeSampleVoices() const noexcept;
    int activeSynthVoices() const noexcept;
    int activeDrumVoices() const noexcept;
    std::uint64_t stolen() const noexcept { return steals; }
    std::uint64_t unhandled() const noexcept { return unhandledEvents; }

private:
    enum class Stage : std::uint8_t
    {
        Idle,
        Attack,
        Decay,
        Sustain,
        Release,
        Fade, // stolen or choked: a short linear fade to silence
    };

    struct Voice
    {
        Stage stage = Stage::Idle;
        VoiceKind kind = VoiceKind::Sample;
        SourceId source = SourceId::BeatMaker;
        std::uint8_t chokeGroup = 0;
        std::uint64_t serial = 0; // start order (oldest first for stealing)
        int delay = 0;            // samples into the next block before it starts
        int chokeAt = -1;         // in the next block: start the fade there
        // Sound
        const SampleData* sample = nullptr;
        double position = 0.0, increment = 1.0;
        double phase = 0.0, phaseIncrement = 0.0;
        dsp::DrumVoice drum;
        // Envelope
        float level = 0.0f, attackStep = 1.0f, decayStep = 1.0f, sustain = 1.0f, releaseStep = 1.0f, fadeStep = 1.0f;
        std::int64_t gateSamples = -1; // release after this many samples (-1: none)
        // Mix
        float gainL = 0.0f, gainR = 0.0f, delaySend = 0.0f, roomSend = 0.0f;
        bool lowpass = false, highpass = false;
        dsp::SvfCoeffs lp, hp;
        std::array<dsp::SvfState, 2> lpState{}, hpState{};
    };

    Voice& allocate(std::span<Voice> pool, std::span<Voice> tails) noexcept;
    void setUp(Voice& v, const ScheduledEvent& e, int offset) noexcept;
    void choke(SourceId source, std::uint8_t group, int offset, const Voice* except) noexcept;
    void render(Voice& v, const VoiceBuses& buses, int numSamples) noexcept;
    bool envelopeStep(Voice& v) noexcept; // false when the voice has finished

    double rate = 48000.0;
    std::array<Voice, kSampleVoices> samples{}, sampleTails{};
    std::array<Voice, kSynthVoices> synths{}, synthTails{};
    std::array<Voice, kDrumVoicesPerSource * kSources> drums{}, drumTails{};
    std::array<DrumSound, kSounds> drumSounds{};
    std::array<const SampleData*, kSounds> sounds{};
    std::uint64_t serialCounter = 0, steals = 0, unhandledEvents = 0;
};

} // namespace devildj::engine
