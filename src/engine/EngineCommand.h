#pragma once

#include "engine/EngineIds.h"
#include "engine/MasterClock.h"
#include "engine/OutputRouter.h"
#include "util/Beatgrid.h"

#include <cstdint>
#include <type_traits>
#include <variant>

namespace devildj::engine
{

// Commands from the UI (and later other producers) to the audio thread (ARCHITECTURE §3.2).
// Every command is a small POD struct; EngineCommand is a std::variant of them, trivially copyable, so it
// travels through util::SpscQueue without allocating.
//
// The full list in ARCHITECTURE §3.2 (LoadTrack, Play, Seek, SetParam, ...) is added as each task needs it.

struct TrackBuffer;

enum class Bus : std::uint8_t
{
    Master = 0,
    Cue = 1, // headphones
};

// Plays a sine tone on one bus, for output checks (P1-02, P1-03, P1-19). Level changes are ramped.
struct SetTestTone
{
    bool on = false;
    float frequencyHz = 440.0f;
    float gainDb = -18.0f;
    Bus bus = Bus::Master;
    float pan = 0.0f; // -1 = left, 0 = centre (constant power), +1 = right
};

// Chooses how master and cue reach the device outputs (P1-03).
struct SetOutputLayout
{
    OutputLayout layout = OutputLayout::StereoMaster;
};

// Puts a decoded track on a deck (P1-05). The buffer is owned by the BufferRegistry on the message thread;
// the engine hands `bufferId` back through popReleasedBuffer() when it no longer uses it.
struct LoadTrack
{
    DeckId deck = 0;
    const TrackBuffer* buffer = nullptr;
    BufferId bufferId = 0;
};

// Empties a deck; its buffer is handed back like a replaced one.
struct UnloadTrack
{
    DeckId deck = 0;
};

// Transport (P1-06; names as in ARCHITECTURE §3.2). `quantized` is honoured from P2-20.
struct Play
{
    DeckId deck = 0;
    bool quantized = false;
};

struct Pause
{
    DeckId deck = 0;
};

struct TogglePlay
{
    DeckId deck = 0;
};

struct CuePress
{
    DeckId deck = 0;
};

struct CueRelease
{
    DeckId deck = 0;
};

struct Seek
{
    DeckId deck = 0;
    double samplePos = 0.0; // in samples of the loaded TrackBuffer (= device rate)
};

// Tempo and pitch (P1-07).
struct SetTempo
{
    DeckId deck = 0;
    double percent = 0.0; // clamped to the deck's range
};

struct NudgeTempo // fine steps (e.g. +-0.1 %)
{
    DeckId deck = 0;
    double deltaPercent = 0.0;
};

struct SetTempoRange
{
    DeckId deck = 0;
    double rangePercent = 10.0; // 6, 10, 16 or 50
};

struct ResetTempo
{
    DeckId deck = 0;
};

struct PitchBend // temporary; send 0 when the bend key is released
{
    DeckId deck = 0;
    double percent = 0.0; // clamped to +-4
};

enum class InterpolationQuality : std::uint8_t
{
    Standard = 0, // cubic Hermite
    High = 1,     // 16-tap sinc
};

struct SetInterpolationQuality
{
    InterpolationQuality quality = InterpolationQuality::Standard;
};

// Key lock (P1-08): tempo changes keep the pitch. Key Shift transposes in semitones (needs key lock).
struct SetKeyLock
{
    DeckId deck = 0;
    bool on = false;
};

struct SetKeyShift
{
    DeckId deck = 0;
    int semitones = 0; // clamped to +-12
};

// Every knob / fader (ARCHITECTURE 3.2); smoothed on the audio thread. Units per ParamId (EngineIds.h).
struct SetParam
{
    ParamId param = ParamId::ChannelFirst;
    float value = 0.0f;
};

// Jog (P1-09): mouse drag on the waveform scratches (playing) or scrubs (paused); the wheel nudges.
// timeSeconds: when the UI saw the event (any steady clock, e.g. juce::Time::getMillisecondCounterHiRes()/1000).
// The deck measures hand speed on that clock, so it doesn't matter which audio block picks the command up.
// Negative = unknown: the audio clock is used instead.
struct ScratchBegin
{
    DeckId deck = 0;
    double timeSeconds = -1.0;
};

struct ScratchMove
{
    DeckId deck = 0;
    double deltaSamples = 0.0; // how far the hand moved since the last move (signed, track samples)
    double timeSeconds = -1.0;
};

struct ScratchEnd
{
    DeckId deck = 0;
};

struct Nudge
{
    DeckId deck = 0;
    double deltaSamples = 0.0; // fine position shift (played out as a small rate offset while playing)
};

// The deck's beatgrid (P2-14), in the deck buffer's samples (the UI scales the library's grid to the buffer's
// rate). A new track clears it; the UI sends the new track's grid once it is loaded. An empty grid = none.
struct SetBeatgrid
{
    DeckId deck = 0;
    util::Beatgrid grid;
};

// Metronome (P2-14): a click on the cue bus on every beat of the deck's grid while it plays (downbeats higher).
struct SetMetronome
{
    DeckId deck = 0;
    bool on = false;
    float levelDb = -6.0f;
};

// Master clock (P2-18): who leads - Auto (the first playing synced deck with a grid), one deck, or the
// internal tempo.
struct SetClockSource
{
    MasterClock::Mode mode = MasterClock::Mode::Auto;
    std::int8_t deck = MasterClock::kInternal; // for Mode::Deck
};

// The internal tempo: the clock's tempo while no deck leads (20..300 BPM).
struct SetInternalTempo
{
    double bpm = 120.0;
};

using EngineCommand =
    std::variant<SetTestTone, SetOutputLayout, LoadTrack, UnloadTrack, Play, Pause, TogglePlay, CuePress, CueRelease,
                 Seek, SetTempo, NudgeTempo, SetTempoRange, ResetTempo, PitchBend, SetInterpolationQuality, SetKeyLock,
                 SetKeyShift, ScratchBegin, ScratchMove, ScratchEnd, Nudge, SetParam, SetBeatgrid, SetMetronome,
                 SetClockSource, SetInternalTempo>;

static_assert(std::is_trivially_copyable_v<EngineCommand>, "EngineCommand must stay trivially copyable");

} // namespace devildj::engine
