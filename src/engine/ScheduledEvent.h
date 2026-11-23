#pragma once

// A note or hit for the voices, at an exact engine sample (ARCHITECTURE §3.7). Producers - the Beat Maker
// sequencer, the code deck - make them ahead of time and push them through their own SPSC queue; the audio thread
// sorts them into its EventBuffer and starts each at its sample offset in the block.

#include <cstdint>
#include <type_traits>

namespace devildj::engine
{

enum class SourceId : std::uint8_t
{
    BeatMaker,
    CodeDeck,
    SamplerSequencer,
};

enum class VoiceKind : std::uint8_t
{
    Sample,
    DrumSynth,
    Synth,
};

constexpr std::int64_t kNow = -1;

struct ScheduledEvent
{
    std::int64_t sampleTime = 0; // absolute engine sample clock; kNow: at the start of the next block (live hits)
    SourceId source = SourceId::BeatMaker;
    VoiceKind kind = VoiceKind::DrumSynth;
    std::uint16_t soundIndex = 0; // the producer's sound (Beat Maker: the pattern's track), resolved off the audio thread
    float note = 60.0f, velocity = 1.0f, gain = 1.0f, pan = 0.5f, speed = 1.0f;
    float cutoffHz = 20000.0f, hpfHz = 0.0f, resonance = 0.0f;
    float attack = 0.0f, decay = 0.0f, sustain = 1.0f, release = 0.0f, durationSeconds = 0.0f;
    float roomSend = 0.0f, delaySend = 0.0f, crush = 0.0f;
    std::uint8_t chokeGroup = 0;
    std::uint32_t highlightToken = 0; // Code Beat's active highlighting; Beat Maker: track << 16 | step
};

static_assert(std::is_trivially_copyable_v<ScheduledEvent>);

} // namespace devildj::engine
