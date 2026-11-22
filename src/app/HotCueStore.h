#pragma once

#include "engine/EngineCommand.h"
#include "engine/EngineIds.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace devildj::library
{
class Connection;
}

namespace devildj::app
{

// The app's side of the hot cues (P3-01): what the library holds for the loaded track, at the deck's rate, plus
// the colours and labels the engine doesn't need. The engine owns the positions while the track plays (it sets
// and deletes them on the pads); the app compares its state with this and writes what changed. Pure functions,
// so the storage round trip is testable without the app.
struct HotCueInfo
{
    bool set = false;
    double start = 0.0, length = 0.0; // deck buffer samples
    std::uint32_t colour = 0;         // 0xRRGGBB
    std::string label;
};
using HotCues = std::array<HotCueInfo, engine::kHotCues>;

// A slot as it goes into the library (file samples).
struct HotCueChange
{
    int slot = 0;
    bool set = false; // false: the slot was emptied
    std::int64_t start = 0, length = 0;
    std::uint32_t colour = 0;
    std::string label;
};

// The pads' colours when a cue is set: the track colour palette in order - A red, B orange, C yellow, D green,
// E aqua, F blue, G purple, H pink; the second page the same.
std::uint32_t defaultHotCueColour(int slot) noexcept;
char hotCueLetter(int slot) noexcept; // 'A'..'P'

// The track's hot cues from the library, at the deck's rate (bufferRate / fileRate).
HotCues loadHotCues(library::Connection& db, std::int64_t trackId, double fileRate, double bufferRate);
// What the engine should hold.
engine::SetHotCues toEngine(engine::DeckId deck, const HotCues& cues) noexcept;

// The engine's slots against `known`: updates `known` (a new cue gets its slot's colour) and returns what to write
// (positions converted to file samples). Positions within half a sample count as the same.
std::vector<HotCueChange> diffHotCues(HotCues& known, const std::array<double, engine::kHotCues>& starts,
                                      const std::array<double, engine::kHotCues>& lengths, double fileRate,
                                      double bufferRate);
// One slot as it is now (after a colour / label change).
HotCueChange changeFor(const HotCues& known, int slot, double fileRate, double bufferRate);
// Writer thread: the changes into the library. False on an error.
bool applyHotCueChanges(library::Connection& db, std::int64_t trackId, const std::vector<HotCueChange>& changes);

// A pad (0..7 on the current page 0 / 1) as the engine command: press / release, or delete (the pad's alternate,
// Shift + pad).
engine::HotCue hotCueForPad(engine::DeckId deck, int pad, int page, bool alternate, bool release) noexcept;

// "Sort by time": the set cues in time order from pad A, colours and labels going with them. The changes to
// write cover every slot.
HotCues sortedByTime(const HotCues& cues);

} // namespace devildj::app
