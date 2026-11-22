#pragma once

#include "matcher/Scorer.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace devildj::library
{
class Connection;
}

namespace devildj::matcher
{

// The SQL side (MATCHER_SPEC §3, §11): one query for the candidates - present, analyzed (BPM and key), in scope,
// rated enough - with their features, tags and recent plays; and what the context needs about A (its features, the
// history counts). Any thread with its own reader connection.
struct Scope
{
    std::int64_t playlistId = 0; // 0 = the whole library
};

std::vector<TrackInfo> loadCandidates(library::Connection& db, const Scope& scope, std::int64_t excludeTrackId,
                                      int minRating, std::int64_t nowUnixSeconds);
// A's TrackInfo, or nothing if it isn't in the library / not analyzed.
std::optional<TrackInfo> loadTrack(library::Connection& db, std::int64_t trackId, std::int64_t nowUnixSeconds);
// History counts into the context (pairs after A, artist pairs, the largest pair count) and the most plays in 30 days.
void loadHistory(library::Connection& db, Context& context, std::int64_t nowUnixSeconds);

} // namespace devildj::matcher
