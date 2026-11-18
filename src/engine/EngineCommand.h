#pragma once

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

// Plays a sine tone on the master output (P1-02 test, P1-03/P1-19 output checks). Gain changes are ramped.
struct SetTestTone
{
    bool on = false;
    float frequencyHz = 440.0f;
    float gainDb = -18.0f;
};

using EngineCommand = std::variant<SetTestTone>;

static_assert(std::is_trivially_copyable_v<EngineCommand>, "EngineCommand must stay trivially copyable");

} // namespace devildj::engine
