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
// Live switching (P4-09): queuePattern / queueSong change what plays from a given beat - the next bar or the
// playing pattern's end (nextBar / patternEnd) - with the new pattern's step 0 exactly there; the plan before it
// is unchanged, so windows already emitted stay right. Song mode plays project.song round and round.
// Synth tracks: their notes (note-on at startTick, lengthTicks long), or with the arpeggiator on, the held notes
// stepped through at its rate and mode (over 1-4 octaves).

#include "beatmaker/Model.h"
#include "engine/ScheduledEvent.h"
#include "engine/Scheduler.h"
#include "util/Fraction.h"

#include <cstdint>
#include <optional>
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
    // Loop one pattern, its step 0 on `startBeat` (forgets any earlier plan).
    void playPattern(int pattern, util::Fraction startBeat) noexcept;
    // Play the project's song (its pattern list, round and round) from `startBeat`; an empty song loops the
    // current pattern.
    void playSong(util::Fraction startBeat) noexcept;
    // From `atBeat` on: loop `pattern` (its step 0 there) / play the song from its start there. Anything planned
    // from `atBeat` or later is replaced.
    void queuePattern(int pattern, util::Fraction atBeat);
    void queueSong(util::Fraction atBeat);
    // Where a switch asked for at `beat` lands: the next bar line (at or after it), or the end of the pass playing
    // then (a pattern's loop, or the song's current pattern).
    static util::Fraction nextBar(util::Fraction beat);
    util::Fraction patternEnd(const Project& project, util::Fraction beat) const;
    // Forget the plan before `beat` (what plays at `beat` stays).
    void prune(util::Fraction beat);

    int currentPattern() const noexcept { return plan.back().pattern; } // the latest planned
    bool songMode() const noexcept { return plan.back().song; }
    // What is planned after `beat` (a queued switch), if anything: its pattern (-1 for the song) and beat.
    struct Queued
    {
        int pattern = -1;
        bool song = false;
        util::Fraction at;
    };
    std::optional<Queued> queuedAfter(util::Fraction beat) const;
    // What plays at `beat`: the pattern and its step (pattern -1 before the start).
    struct Position
    {
        int pattern = -1;
        int step = -1;
    };
    Position position(const Project& project, util::Fraction beat) const;

    // Appends the events in [begin, end) to `out`, sorted by beat.
    void emit(const Project& project, util::Fraction begin, util::Fraction end, std::vector<BeatEvent>& out) const;

    // The hash behind probability (0..99), for tests.
    static int chance(std::uint32_t seed, int pattern, int track, int step, std::int64_t loop) noexcept;

private:
    struct Placement
    {
        util::Fraction from;  // in force from this beat
        int pattern = 0;      // the pattern looped (or the song's fallback when it is empty)
        util::Fraction start; // its step 0 / the song's start
        bool song = false;
    };
    void emitPlacement(const Project& project, const Placement& p, util::Fraction begin, util::Fraction end,
                       std::vector<BeatEvent>& out) const;
    void emitPass(const Project& project, int patternIndex, util::Fraction passStart, std::int64_t loop,
                  util::Fraction begin, util::Fraction end, std::vector<BeatEvent>& out) const;
    const Placement& at(util::Fraction beat) const; // the placement in force at `beat` (the first one before)
    static std::vector<int> songList(const Project& project);

    std::vector<Placement> plan{Placement{}};
};

// The event for the voices: its sample, the Beat Maker as its source.
engine::ScheduledEvent toScheduled(const BeatEvent& e, const engine::ClockSnapshot& clock);

} // namespace devildj::beatmaker
