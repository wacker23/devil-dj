#pragma once

// The Beat Maker's event source (BEATMAKER_SPEC §2): turns the playing pattern (or the song) into timed events for
// a window of master-clock beats. Called from the Beat Maker timer every ~25 ms with a ScheduleCursor window
// (~100 ms ahead); the events are converted to engine samples with a ClockSnapshot and pushed to its EventQueue.
//
// Timing, in beats from the pattern's start: step k at k / stepsPerBeat; odd steps later by
// swing x step / 2; then micro-timing (+-50 % of a step); a ratchet splits the step into equal hits, each 10 %
// quieter. An event belongs to the window its own (shifted) time falls in, so windows of any size give every
// event exactly once and none is pushed into the past by a negative micro-timing.
//
// Probability: hash(seed, pattern, track, step, loop) mod 100 < probability - the same every time for a seed.
// Solo / mute: when any track of the pattern is soloed only soloed tracks play; muted tracks never do.
// Synth tracks: their notes (note-on at startTick, lengthTicks long), or with the arpeggiator on, the held notes
// stepped through at its rate and mode (over 1-4 octaves).

#include "beatmaker/Model.h"
#include "engine/ScheduledEvent.h"
#include "engine/Scheduler.h"
#include "util/Fraction.h"

#include <cstdint>
#include <vector>

namespace devildj::beatmaker
{

struct BeatEvent
{
    util::Fraction beat;     // master-clock beat
    util::Fraction duration; // beats (synth notes; a drum hit's is its step)
    engine::VoiceKind kind = engine::VoiceKind::DrumSynth;
    int pattern = 0;
    int track = 0; // index into the pattern's drums or synths
    int step = 0;  // drum step, or the note's / arp step's index
    std::uint8_t pitch = 60;
    float velocity = 1.0f; // 0..1
    float gain = 1.0f;     // from the track's volume
    float pan = 0.5f, delaySend = 0.0f, reverbSend = 0.0f;
    std::uint8_t chokeGroup = 0;
};

class Sequencer
{
public:
    // Loop one pattern, its step 0 on `startBeat`.
    void playPattern(int pattern, util::Fraction startBeat) noexcept;
    // Play the project's song (its pattern list, round and round) from `startBeat`; an empty song loops the
    // current pattern.
    void playSong(util::Fraction startBeat) noexcept;
    int currentPattern() const noexcept { return pattern; }
    bool songMode() const noexcept { return song; }

    // Appends the events in [begin, end) to `out`, sorted by beat.
    void emit(const Project& project, util::Fraction begin, util::Fraction end, std::vector<BeatEvent>& out) const;

    // The hash behind probability (0..99), for tests.
    static int chance(std::uint32_t seed, int pattern, int track, int step, std::int64_t loop) noexcept;

private:
    void emitPass(const Project& project, int patternIndex, util::Fraction passStart, std::int64_t loop,
                  util::Fraction begin, util::Fraction end, std::vector<BeatEvent>& out) const;

    int pattern = 0;
    bool song = false;
    util::Fraction start = 0;
};

// The event for the voices: its sample, the Beat Maker as its source.
engine::ScheduledEvent toScheduled(const BeatEvent& e, const engine::ClockSnapshot& clock);

} // namespace devildj::beatmaker
