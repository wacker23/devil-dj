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
    double rate = 1.0; // effective playback rate = (1 + tempo) x (1 + bend)
    double tempoPercent = 0.0;
    double tempoRangePercent = 10.0;
    double pitchBendPercent = 0.0; // current (slewed) bend
    bool keyLock = false;
    std::int8_t transposeSemis = 0; // Key Shift
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
    std::uint32_t buffersAwaitingRelease = 0; // replaced buffers not yet handed back (queue was full)
};

static_assert(std::is_trivially_copyable_v<EngineState>, "EngineState must stay trivially copyable");

} // namespace devildj::engine
