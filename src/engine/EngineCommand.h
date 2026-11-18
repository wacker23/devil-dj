#pragma once

#include "engine/OutputRouter.h"

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

using EngineCommand = std::variant<SetTestTone, SetOutputLayout>;

static_assert(std::is_trivially_copyable_v<EngineCommand>, "EngineCommand must stay trivially copyable");

} // namespace devildj::engine
