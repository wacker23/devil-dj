#pragma once

#include "util/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace devildj::library
{
class Connection;
}

namespace devildj::app
{

// Memory cues and memory loops (P3-02): up to 10 of each per track, kept in the library's `cues` (kinds
// MemoryCue / MemoryLoop, no pad slot). Unlike hot cues they aren't on pads: the deck's list panel and the call
// keys (previous / next from the playhead) reach them, and a call makes one the deck's CUE point. A memory loop is
// called like a cue for now; looping it comes with loops (P3-03). Positions here are in deck buffer samples.
inline constexpr int kMaxMemoryCues = 10;
inline constexpr int kMaxMemoryLoops = 10;

struct MemoryCue
{
    std::int64_t id = 0;
    bool loop = false;
    double start = 0.0, length = 0.0; // deck buffer samples
    std::optional<std::uint32_t> colour;
    std::string label;
};

// The track's memory cues and loops, by position.
std::vector<MemoryCue> loadMemoryCues(library::Connection& db, std::int64_t trackId, double fileRate, double bufferRate);

// Writer thread. Adds one (fails when the track has 10 of that kind, or one starts within 10 ms of it already);
// returns its id.
util::Result<std::int64_t> addMemoryCue(library::Connection& db, std::int64_t trackId, bool loop, double start,
                                        double length, double fileRate, double bufferRate,
                                        const std::string& label = {});
bool renameMemoryCue(library::Connection& db, std::int64_t id, const std::string& label);
bool recolourMemoryCue(library::Connection& db, std::int64_t id, std::optional<std::uint32_t> colour);
bool removeMemoryCue(library::Connection& db, std::int64_t id);

// Call: the next one after the playhead / the one before it (a cue right at the playhead counts as passed, so
// "previous" goes further back). nullopt when there is none that way.
std::optional<MemoryCue> nextMemoryCue(const std::vector<MemoryCue>& cues, double position);
std::optional<MemoryCue> previousMemoryCue(const std::vector<MemoryCue>& cues, double position);

} // namespace devildj::app
