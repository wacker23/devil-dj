#pragma once

#include "engine/EngineCommand.h"
#include "engine/EngineState.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <vector>

namespace devildj::ui
{

// The pad grid's modes (P3-06; KEYMAP: F1-F4, Shift+F1-F4). Message thread. Pads are numbered 0..7: the top row
// 0-3 (keys Q W E R / U I O P), the bottom row 4-7 (A S D F / J K L ;).
//   Hot Cue   - hot cues A-H / I-P (P3-01; F1 again switches the page)
//   Beat Loop - a loop of 1/4, 1/2, 1, 2 (top) / 4, 8, 16, 32 beats (bottom); the same pad again exits it, another
//               size resizes it from its start
//   Beat Jump - back / forward 1, 2 (top) / 4, 8 beats (bottom): odd pads back, even pads forward
//   Key Shift - the key in semitones: 0, +1, +2, +3 (top) / -4, -3, -2, -1 (bottom), key lock on; Shift+pad: 0
//   Keyboard  - hot cue A played at those pitches (press plays it, release from pause returns, like a held hot cue)
//   Slicer    - the 8 slices of the slicer domain (8 / 16 / 32 beats, P3-07): held, a slice rolls on the grid;
//               let go, the deck is where the track would be. Shift + pad 1 / 2 / 3: domain 8 / 16 / 32 beats.
//   Pad FX, Sampler come with their tasks (P3-12, P3-13): until then their pads do nothing.
enum class PadMode : std::uint8_t
{
    HotCue,
    BeatLoop,
    BeatJump,
    Slicer,
    KeyShift,
    Keyboard,
    PadFx,
    Sampler,
};
inline constexpr int kPadModeCount = 8;

const char* padModeName(PadMode mode) noexcept;
// The task a mode is waiting for ("P3-07"), or nullptr when it works.
const char* padModeComesWith(PadMode mode) noexcept;
inline bool padModeAvailable(PadMode mode) noexcept { return padModeComesWith(mode) == nullptr; }

double beatLoopBeats(int pad) noexcept; // 1/4 ... 32
double beatJumpBeats(int pad) noexcept; // -1, +1, -2, +2, -4, +4, -8, +8
int padSemitones(int pad) noexcept;     // 0, +1, +2, +3, -4, -3, -2, -1

// How a pad looks: its text, its colour (RGB), lit (full colour) or not (dimmed), usable or not.
struct PadLook
{
    juce::String text;
    std::uint32_t colour = 0;
    bool lit = false;
    bool enabled = true;
};
// For every mode but Hot Cue (whose pads show the cues' own colours and names). `held`: the pad is down.
PadLook padLook(PadMode mode, int pad, const engine::DeckState& deck, bool held);

// What a pad press / release does. `alternate`: Shift + pad. `hotCuePage`: 0 = A-H, 1 = I-P.
std::vector<engine::EngineCommand> padCommands(PadMode mode, engine::DeckId deck, int pad, int hotCuePage,
                                               bool alternate, bool release, const engine::DeckState& state);

} // namespace devildj::ui
