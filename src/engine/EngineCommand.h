#pragma once

#include "dsp/fx/Effect.h"
#include "engine/EngineIds.h"
#include "engine/Sequencer.h"
#include "engine/MasterClock.h"
#include "engine/OutputRouter.h"
#include "engine/Voices.h"
#include "util/Beatgrid.h"

#include <array>
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

// Transport (P1-06; names as in ARCHITECTURE §3.2). `quantized` (P2-20) quantizes this one command even when
// the global quantize is off.
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
    bool quantized = false; // P2-20: see Play
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

// Quantize (P2-20): with it on, play from pause starts on the master clock's next grid point, a cue set while
// paused snaps to the nearest grid point, and a jump while playing happens as the playhead crosses its next grid
// point and lands on one (keeping the phase). Resolution in beats: 1/8, 1/4, 1/2 or 1.
struct SetQuantize
{
    bool on = false;
    float beats = 1.0f;
};

// Hot cues (P3-01). The app loads a track's hot cues (buffer samples; length > 0 = a hot loop, its loop comes with
// P3-03) after the track; the engine owns them while it plays and reports them in DeckState::hotCues, so the app
// saves what changes. Press on an empty slot sets it at the playhead (quantize: the nearest grid point); press on
// a set one jumps there - while playing at once, or with quantize on the next grid point (phase kept); while
// paused it plays from there for as long as the pad is held (release goes back and pauses; Play while holding
// keeps playing). Delete empties the slot.
struct HotCueSlot
{
    bool set = false;
    double start = 0.0;
    double length = 0.0;
};

// Loops (P3-03). Lengths in grid beats (1/64 ... 512); positions in buffer samples. With quantize on, a loop's
// in point (auto loop, loop in) lands on the grid point at or before the playhead and its out point on a grid
// point; without a grid, auto loops need one (manual in / out works anywhere).
enum class LoopOp : std::uint8_t
{
    Auto,   // toggle: a loop of `beats` from here, or exit the one that plays
    In,     // the in point here (the next Out closes the loop)
    Out,    // the out point here: loop from the in point
    Halve,  // the loop half as long (or the auto-loop size, when no loop plays)
    Double, // twice as long
    Reloop, // the last loop again, from its start
    Exit,   // stop looping, play on
    Set,    // a stored loop (hot / memory loop): `start` .. `end`, and jump to its start
    Beat,   // a beat-loop pad (P3-06): a loop of `beats` from here; one of that size plays: exit; another: resize
};

struct Loop
{
    DeckId deck = 0;
    LoopOp op = LoopOp::Auto;
    double beats = 4.0;
    double start = 0.0, end = 0.0; // Set
};

// P3-02: the deck's CUE point (a memory cue called), in buffer samples. The playhead doesn't move (send a Seek).
struct SetCuePoint
{
    DeckId deck = 0;
    double samplePos = 0.0;
};

struct SetHotCues
{
    DeckId deck = 0;
    std::array<HotCueSlot, kHotCues> cues{};
};

enum class HotCueOp : std::uint8_t
{
    Press,
    Release,
    Delete,
};

struct HotCue
{
    DeckId deck = 0;
    std::uint8_t slot = 0; // 0..15
    HotCueOp op = HotCueOp::Press;
};

// Beat jump (P2-21): exactly `beats` grid beats forward (+) or back (-), so the phase is kept. With quantize on
// and the deck playing it happens on the next grid point (see SetQuantize); another jump pressed while one waits
// adds to it. No beatgrid: nothing happens. (While a loop plays it moves the loop: P3-03.)
struct BeatJump
{
    DeckId deck = 0;
    double beats = 0.0;
};

// A fine jump by time (P2-21: +-5 ms), at once, never quantized.
struct JumpBy
{
    DeckId deck = 0;
    double seconds = 0.0;
};

// Slip mode (P3-04): loops, held hot cues, scratching and pause run a shadow playhead; ending them returns to it.
struct SetSlip
{
    DeckId deck = 0;
    bool on = false;
};

// Reverse (P3-05): the deck plays backwards while on (a slip reason with slip on).
struct SetReverse
{
    DeckId deck = 0;
    bool on = false;
};

// Stopping like a turntable (P3-05): instant (= Pause), vinyl brake, or spin-back; the times come from SetStopStyle.
enum class StopKind : std::uint8_t
{
    Instant,
    Brake,
    SpinBack,
};

struct Stop
{
    DeckId deck = 0;
    StopKind kind = StopKind::Brake;
};

// How play / pause (TogglePlay) stops a playing deck, and the brake / spin-back times in seconds.
struct SetStopStyle
{
    StopKind pause = StopKind::Instant;
    float brakeSeconds = 1.0f;
    float spinBackSeconds = 1.0f;
};

// Slicer (P3-07): the deck's slicer domain on / off and its length (8, 16 or 32 beats, 8 slices). The domain
// starts on the bar at or before the playhead and moves on by its length as the track plays past its end.
struct SetSlicer
{
    DeckId deck = 0;
    bool on = false;
    double beats = 8.0;
};

// A slicer pad (0..7): while held, its slice rolls (loops) from the next slice boundary; when the last pad is
// let go, the deck returns to where the track would be (a slip hold, also with slip off).
struct SlicerPad
{
    DeckId deck = 0;
    std::uint8_t pad = 0;
    bool release = false;
};

// FX units (P3-08). An effect is created and prepared on the message thread (AudioEngine::installEffect) and
// handed over by pointer; the one it replaces comes back through the engine's release queue.
struct FxSetEffect
{
    std::uint8_t unit = 0, slot = 0;
    dsp::fx::Effect* effect = nullptr; // nullptr: empty the slot
};

enum class FxOp : std::uint8_t
{
    On,     // slot on (value != 0) / off (with its tail)
    Wet,    // slot dry / wet 0..1
    Beats,  // slot beat length (beats)
    Param,  // slot parameter `index` (0..1)
    Target, // unit target (value = FxTarget)
    Mode,   // unit mode (0 multi, 1 single)
    TapBpm, // unit tempo: a tapped BPM (P3-09), or 0 = the master clock's
};

struct FxCommand
{
    std::uint8_t unit = 0;
    FxOp op = FxOp::On;
    std::uint8_t slot = 0;
    std::uint8_t index = 0;
    float value = 0.0f;
};

// Color FX (P3-10): what every channel's FILTER knob does (dsp::fx::ColorFx::Type; 0 = the filter).
struct SetColorFx
{
    std::uint8_t type = 0;
};

// Release FX (P3-11) on a deck's channel: press (hold) with a kind (dsp::fx::ReleaseFx::Kind) and a beat length;
// release: the music is cut and the effect ends it; then the deck pauses.
struct ReleaseFxCommand
{
    DeckId deck = 0;
    std::uint8_t kind = 0;
    bool press = true;
    float beats = 1.0f;
};

// Pad FX (P3-12): the effect behind a pad of a deck (handed over by pointer, like FxSetEffect), and its settings
// / on-off (op On, Wet, Beats, Param as for FxCommand).
struct PadFxSetEffect
{
    DeckId deck = 0;
    std::uint8_t pad = 0;
    dsp::fx::Effect* effect = nullptr;
};

struct PadFxCommand
{
    DeckId deck = 0;
    std::uint8_t pad = 0;
    FxOp op = FxOp::On;
    std::uint8_t index = 0;
    float value = 0.0f;
};

// Sampler (P3-13): a sample into a slot (0..31; nullptr = empty it; the old buffer comes back through the release
// queue), a pad press / release (quantized to the master clock when quantize is on), the slot's settings.
struct SamplerLoad
{
    std::uint8_t slot = 0;
    const TrackBuffer* buffer = nullptr;
    BufferId id = 0;
    float bpm = 0.0f; // the sample's tempo (0 = unknown: no sync)
};

struct SamplerPad
{
    std::uint8_t slot = 0;
    bool release = false;
};

enum class SamplerOp : std::uint8_t
{
    Mode,    // 0 one-shot, 1 loop, 2 gate
    Gain,    // 0..2
    Sync,    // 0 / 1
    Stop,    // this slot
    StopAll, // every slot
};

struct SamplerSet
{
    std::uint8_t slot = 0;
    SamplerOp op = SamplerOp::Mode;
    float value = 0.0f;
};

// Sequencer (P3-15): the sampler's pad presses recorded into a loop on the master clock's bars.
enum class SequencerOp : std::uint8_t
{
    Clear,     // empty, value = bars (1 / 2 / 4); a load's SequencerEvents follow
    Bars,      // the length (keeps what fits)
    Play,      // value != 0: play, else stop
    Record,    // value != 0: record (and play) - overdub; 0: stop recording, play on
    Erase,     // every event
    EraseSlot, // the slot's events
    Mute,      // value != 0: the slot's events are skipped
    Metronome, // value != 0: a click in the headphones on every beat while recording
};

struct SequencerCommand
{
    SequencerOp op = SequencerOp::Play;
    std::uint8_t slot = 0;
    float value = 0.0f;
};

// Events for the sequence in the engine (after a Clear), a chunk at a time.
struct SequencerEvents
{
    static constexpr int kChunk = 16;
    std::uint8_t count = 0;
    std::array<SequenceEvent, kChunk> events{};
};

// Auto-transition (P3-17): from the playing deck to another over `beats` (8 / 16 / 32), starting on the master
// clock's beat `startBeat` (< 0: its next 16-beat boundary): the incoming deck starts there (synced, from where it
// is cued), the crossfader moves across (or, without A / B sides, the outgoing fader comes down), the low EQs swap
// at the middle; the style adds a high-pass sweep on the outgoing deck or ends it on an echo. The outgoing deck stops
// at the end. CancelTransition stops it where it is (EQs back to normal).
enum class TransitionStyle : std::uint8_t
{
    Blend,
    FilterSweep,
    EchoOut,
};

struct StartTransition
{
    DeckId from = 0, to = 1;
    float beats = 16.0f;
    TransitionStyle style = TransitionStyle::Blend;
    double startBeat = -1.0;
};

struct CancelTransition
{
};

// The Beat Maker (P4-10): the voices' sound tables (index = pattern x 16 + track), and a stop that drops the
// events still waiting and fades out what sounds.
struct SetDrumSound
{
    std::uint16_t index = 0;
    DrumSound sound;
};
struct SetSynthSound
{
    std::uint16_t index = 0;
    dsp::SynthParams sound;
};
struct BeatMakerStop
{
};
// P4-13: a drum track's user sample (index = pattern x 16 + track); nullptr: none (a missing file plays nothing).
// The sample stays alive while the app runs (app::SampleBank), so the audio thread may keep the pointer.
struct SetSampleSound
{
    std::uint16_t index = 0;
    const SampleData* sample = nullptr;
};

// Beat sync (P2-19): the deck follows the master clock's tempo (half / double aware) and phase.
struct SetSync
{
    DeckId deck = 0;
    bool on = false;
};

using EngineCommand =
    std::variant<SetTestTone, SetOutputLayout, LoadTrack, UnloadTrack, Play, Pause, TogglePlay, CuePress, CueRelease,
                 Seek, SetTempo, NudgeTempo, SetTempoRange, ResetTempo, PitchBend, SetInterpolationQuality, SetKeyLock,
                 SetKeyShift, ScratchBegin, ScratchMove, ScratchEnd, Nudge, SetParam, SetBeatgrid, SetMetronome,
                 SetClockSource, SetInternalTempo, SetSync, SetQuantize, BeatJump, JumpBy, SetHotCues, HotCue,
                 SetCuePoint, Loop, SetSlip, SetReverse, Stop, SetStopStyle, SetSlicer, SlicerPad, FxSetEffect, FxCommand, SetColorFx, ReleaseFxCommand, PadFxSetEffect, PadFxCommand,
                 SamplerLoad, SamplerPad, SamplerSet, SequencerCommand, SequencerEvents,
                 StartTransition, CancelTransition, SetDrumSound, SetSynthSound, BeatMakerStop, SetSampleSound>;

static_assert(std::is_trivially_copyable_v<EngineCommand>, "EngineCommand must stay trivially copyable");

} // namespace devildj::engine
