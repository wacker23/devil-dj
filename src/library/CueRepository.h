#pragma once

#include "util/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace devildj::library
{

class Connection;

enum class CueKind
{
    HotCue = 0,
    MemoryCue = 1,
    MemoryLoop = 2,
    HotLoop = 3,
    MixIn = 4,
    MixOut = 5,
};

constexpr int kHotCueSlots = 16;

// One row of `cues`. Positions are in samples of the track's own sample rate.
struct Cue
{
    std::int64_t id = 0;
    std::int64_t trackId = 0;
    CueKind kind = CueKind::HotCue;
    std::optional<int> slot; // 0..15 for hot cues and hot loops (pads A..P); none for the others
    std::int64_t startSample = 0;
    std::int64_t lengthSamples = 0; // > 0 for loops, 0 otherwise
    std::string label;
    std::optional<std::uint32_t> color; // 0xRRGGBB

    bool isHot() const noexcept { return kind == CueKind::HotCue || kind == CueKind::HotLoop; }
    bool isLoop() const noexcept { return kind == CueKind::MemoryLoop || kind == CueKind::HotLoop; }
    friend bool operator==(const Cue&, const Cue&) = default;
};

// Cues and loops of a track (P2-03). Same rules as TrackRepository. Writes note a Cues change with the track id.
class CueRepository
{
public:
    explicit CueRepository(Connection& connection) noexcept;

    // A hot cue / hot loop replaces whatever was in its slot, so a pad never holds two.
    util::Result<std::int64_t> add(const Cue& cue);
    std::optional<Cue> get(std::int64_t id);
    std::vector<Cue> forTrack(std::int64_t trackId); // hot cues by slot, then the rest by position
    std::optional<Cue> hotCue(std::int64_t trackId, int slot);
    bool update(const Cue& cue); // same slot rule as add(); the track can't change
    bool remove(std::int64_t id);
    bool removeAllForTrack(std::int64_t trackId);

    const std::string& error() const noexcept { return lastError; }

private:
    bool fail(std::string message);
    std::string validate(const Cue& cue);
    bool clearSlot(const Cue& cue); // removes another cue in cue's hot slot
    Connection& db;
    std::string lastError;
};

} // namespace devildj::library
