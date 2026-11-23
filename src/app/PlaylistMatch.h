#pragma once

// Matching a streaming playlist's track list to the owner's own library (P4-21): only names and durations are
// compared - nothing is downloaded or played.
//
// Each title is split into its base ("Strobe") and its version ("club mix", "radio edit", "<remixer> remix") with
// featuring credits ("feat. X", "ft. X", "(with X)") moved to the artists. Scores (0..1): the base titles (equal, or
// word overlap), the artists (any in common), the version (the same, or both plain - a remix is not the original),
// the duration (within 3 s). The best one is taken when it is clearly best; close calls are "ambiguous" (the owner
// picks); nothing good enough is "missing". A word index over the library keeps it fast on 20,000 tracks.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace devildj::app
{

struct LibraryTrack
{
    std::int64_t id = 0;
    std::string title, artist, album;
    double seconds = 0.0; // 0 = unknown
};

struct RemoteTrack
{
    std::string title;
    std::vector<std::string> artists;
    std::string album;
    double seconds = 0.0;
    std::string isrc; // kept for the report (the library has none to compare with)
};

struct TrackMatch
{
    enum class Kind
    {
        Matched,
        Ambiguous,
        Missing,
    };
    Kind kind = Kind::Missing;
    std::int64_t trackId = 0;                                  // Matched: the track; Ambiguous: the best guess
    double score = 0.0;
    std::vector<std::pair<std::int64_t, double>> candidates;   // Ambiguous: the close ones, best first (up to 5)
};

// A title's parts, for tests and the report.
struct TitleParts
{
    std::string base, version;       // lower case, no punctuation; version empty = the plain / original one
    std::vector<std::string> featuring;
};
TitleParts splitTitle(const std::string& title);
std::string normalizeName(const std::string& text); // lower case, accents folded, punctuation to spaces, single spaces
std::vector<std::string> splitArtists(const std::string& artists);

class PlaylistMatcher
{
public:
    explicit PlaylistMatcher(const std::vector<LibraryTrack>& library);
    TrackMatch match(const RemoteTrack& track) const;
    double score(const RemoteTrack& remote, std::int64_t libraryTrackId) const; // 0 if no such track

    static constexpr double kSure = 0.85, kMaybe = 0.5, kClearMargin = 0.1;

private:
    struct Entry
    {
        std::int64_t id = 0;
        TitleParts title;
        std::vector<std::string> artists;
        std::vector<std::string> titleWords, artistWords; // sorted, unique
        double seconds = 0.0;
    };
    struct Query; // a remote track, split once
    double scoreEntry(const Query& q, const Entry& e, double titleOverlap) const;
    std::vector<Entry> entries;
    std::unordered_map<std::string, std::vector<int>> byWord; // base-title word -> entries
    std::unordered_map<std::int64_t, int> byId;
    mutable std::vector<int> shared;  // per entry: title words in common with the query (zeroed after each match)
    mutable std::vector<int> touched; // the entries with shared > 0
};

} // namespace devildj::app
