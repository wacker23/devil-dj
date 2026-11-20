#pragma once

#include "util/Result.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace devildj::library
{

class Connection;

// Play history (P2-23; ARCHITECTURE §6 `history`). A session is a History playlist named by the date and time it
// started ("2026-11-20 21:13"); its tracks are what was played, in order. Each play is also a `history` row with
// the track played just before it in the session (`previous_track_id`): the transitions the Track Matcher counts
// (MATCHER_SPEC §4.9). Writer thread for the writes; any connection for the reads.
struct HistoryEntry
{
    std::int64_t id = 0;
    std::int64_t sessionId = 0; // the History playlist
    std::int64_t trackId = 0;
    std::int64_t playedAt = 0;  // unix seconds
    int deck = 0;
    std::optional<std::int64_t> previousTrackId;
};

struct Transition
{
    std::int64_t fromTrackId = 0, toTrackId = 0;
    std::int64_t playedAt = 0;
};

class HistoryRepository
{
public:
    explicit HistoryRepository(Connection& connection) noexcept;

    // A new session: its History playlist ("YYYY-MM-DD HH:MM" in local time, from `startedAt` unix seconds).
    util::Result<std::int64_t> startSession(std::int64_t startedAt);
    // A track played in a session: appended to its playlist; the transition from the session's last track.
    util::Result<std::int64_t> recordPlay(std::int64_t sessionId, std::int64_t trackId, std::int64_t playedAt, int deck);

    std::vector<HistoryEntry> entries(std::int64_t sessionId); // in play order
    std::vector<std::int64_t> sessions();                        // History playlists, newest first
    // Every transition A -> B played (all sessions), oldest first; and how often A -> B was played.
    std::vector<Transition> transitions();
    int transitionCount(std::int64_t fromTrackId, std::int64_t toTrackId);
    std::vector<std::pair<std::int64_t, int>> playedAfter(std::int64_t fromTrackId); // B and its count, most first

    // A session as a text tracklist: "01. 21:14  Artist - Title" per line (UTF-8). Returns the lines written.
    util::Result<int> exportTracklist(std::int64_t sessionId, const std::filesystem::path& file);
    std::vector<std::string> tracklist(std::int64_t sessionId);

    const std::string& error() const noexcept { return lastError; }

private:
    Connection& db;
    std::string lastError;
};

} // namespace devildj::library
