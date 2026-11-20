#pragma once

#include "engine/EngineCommand.h"
#include "engine/OutputRouter.h"

#include <array>
#include <cstdint>
#include <type_traits>

namespace devildj::engine
{

// One deck as the UI sees it (ARCHITECTURE §3.3; more fields arrive with tempo, sync, loops, ...).
struct DeckState
{
    bool loaded = false;
    BufferId bufferId = 0;
    int lengthSamples = 0;

    bool playing = false;    // playing or cue preview
    bool cuePreview = false; // cue button held from pause
    bool atEnd = false;
    double positionSamples = 0.0;
    double cuePointSamples = 0.0;
    double rate = 1.0; // effective playback rate now (tempo x bend, or the scratch/nudge rate)
    double tempoPercent = 0.0;
    double tempoRangePercent = 10.0;
    double pitchBendPercent = 0.0; // current (slewed) bend
    bool keyLock = false;
    bool scratching = false;        // jog held (or easing back after release)
    std::int8_t transposeSemis = 0; // Key Shift

    // Beatgrid (P2-14): where the playhead is on it.
    bool hasGrid = false;
    bool gridLocked = false;
    double beat = 0.0;    // beat index at the playhead (fractional)
    int barBeat = 1;      // 1..4: which beat of the bar the current beat is
    double gridBpm = 0.0; // the grid's tempo here (the track's own, without the tempo fader)
    bool metronome = false;
};

// Mixer channel settings as the audio thread has them (targets; the sound follows within 20 ms).
struct ChannelState
{
    float trimDb = 0.0f;
    std::array<float, 3> eqDb{}; // low, mid, high; -inf = kill
    float filter = 0.5f;
    float filterResonance = 0.3f;
    float fader = 1.0f;
    std::uint8_t eqMode = 0;           // 0 EQ, 1 isolator
    std::uint8_t faderCurve = 0;       // 0 smooth, 1 linear, 2 sharp
    std::uint8_t crossfaderAssign = 1; // 0 A, 1 THRU, 2 B
    bool cue = false;                  // CUE button: this channel (pre-fader) is in the headphones

    // Meters, linear (0 dBFS = 1): pre-fader, so they show what trim and EQ do. Peaks fall at 20 dB/s.
    float peakL = 0.0f, peakR = 0.0f, rms = 0.0f;
};

// Snapshot published by the audio thread once per block through util::TripleBuffer (ARCHITECTURE §3.3).
// The UI reads the latest one at display rate. Fields from §3.3 (decks, meters, master clock, ...) are
// added as each task needs them.
struct EngineState
{
    std::uint64_t blockCounter = 0; // blocks processed since the device started
    double hostTimeSeconds = 0.0;   // juce::Time::getMillisecondCounterHiRes() / 1000 when published
    double sampleRate = 0.0;
    int blockSize = 0; // samples in the last block

    float cpuLoad = 0.0f;     // share of the block time spent in the callback, smoothed (0..1+)
    float cpuLoadPeak = 0.0f; // slowly decaying peak of the per-block load
    std::uint32_t xruns = 0;  // blocks whose processing took longer than the block duration

    std::uint64_t commandsProcessed = 0;

    OutputLayout outputLayout = OutputLayout::StereoMaster;
    int outputChannels = 0; // active device outputs in the last block
    bool cueRouted = false; // the cue bus is audible with this layout and device

    bool testToneOn = false;
    float testToneHz = 0.0f;
    Bus testToneBus = Bus::Master;

    std::array<DeckState, kMaxDecks> decks{};
    std::array<ChannelState, kMaxChannels> channels{};

    // Mixer master section (P1-11).
    float crossfader = 0.5f;
    std::uint8_t crossfaderCurve = 0; // 0 smooth, 1 linear, 2 cut
    float masterGainDb = 0.0f;
    float masterPeakL = 0.0f, masterPeakR = 0.0f, masterRms = 0.0f; // after the limiter, linear
    float limiterGainReductionDb = 0.0f;                            // largest in the last block (>= 0)
    int masterLatencySamples = 0;                                   // limiter look-ahead

    // Headphones (P1-12): cue bus = channels with CUE on, mixed with the master, then the headphone level.
    float cueMix = 0.5f;
    float headphoneLevelDb = 0.0f;
    float cuePeakL = 0.0f, cuePeakR = 0.0f;   // headphone signal after its limiter, linear
    std::uint32_t buffersAwaitingRelease = 0; // replaced buffers not yet handed back (queue was full)

    // Master clock (P2-18), at the end of the last block.
    std::int64_t sampleClock = 0; // samples rendered since prepare (the scheduler's time base)
    double masterBpm = 120.0;
    double masterBeat = 0.0;
    std::int8_t masterSource = -1; // the leading deck, -1 = the internal tempo
    std::uint8_t clockMode = 0;    // MasterClock::Mode
    double internalBpm = 120.0;
};

static_assert(std::is_trivially_copyable_v<EngineState>, "EngineState must stay trivially copyable");

} // namespace devildj::engine
