#pragma once

// The playlist import (P4-21): a Spotify playlist's track list matched to the library, then made a library playlist
// "<name> (Spotify)" of the matches (in the playlist's order; the ambiguous ones as the owner chose), with the rest
// listed for the owner (copy / save as text). Spotify audio is never streamed or cached - only names are read.

#include "app/PlaylistMatch.h"
#include "util/Result.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <string>
#include <vector>

namespace devildj::library
{
class Connection;
}

namespace devildj::app
{

// The library's tracks to match against (not the missing ones).
std::vector<LibraryTrack> loadLibraryTracks(library::Connection& db);

struct ImportPlan
{
    std::string name;                 // the Spotify playlist's
    std::vector<RemoteTrack> tracks;
    std::vector<TrackMatch> matches;  // one per track
    std::vector<std::int64_t> chosen; // one per track: the library track to use (0: none) - set from the matches,
                                      // the ambiguous ones' best guess until the owner picks
    int matched() const;
    int ambiguous() const;
    int missing() const;
};

ImportPlan planImport(const std::string& name, std::vector<RemoteTrack> tracks, const PlaylistMatcher& matcher);
// The playlist made from the chosen tracks (writer connection): its id. Named "<name> (Spotify)" (2, 3 ... if taken).
util::Result<std::int64_t> createImportedPlaylist(library::Connection& db, const ImportPlan& plan);
// The tracks not in the library, one per line: "Artist, Artist - Title (Album) [ISRC]".
juce::String missingReport(const ImportPlan& plan);

} // namespace devildj::app
