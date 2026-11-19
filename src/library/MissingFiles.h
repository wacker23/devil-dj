#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace devildj::library
{

class Database;

// Missing files (P2-10). The library keeps a track's id when its file moves, so cues, playlists, tags and
// ratings stay linked; only `path` (and `missing`) change.

struct MissingScanResult
{
    int checked = 0;
    int missing = 0;       // missing now
    int newlyMissing = 0;  // were there last time
    int foundAgain = 0;    // were missing, are back
    bool cancelled = false;
};

// Checks every track's file and updates tracks.missing (both ways). Blocks: run it on a worker thread.
MissingScanResult scanForMissingFiles(Database& db, const std::function<bool()>& isCancelled = {},
                                      const std::function<void(int done, int total)>& onProgress = {});

struct RelocateResult
{
    int lookedFor = 0;  // missing tracks considered
    int relocated = 0;  // found and linked to their new place
    int notFound = 0;
    int filesIndexed = 0;
    bool cancelled = false;
    std::vector<std::string> notes; // ambiguities and conflicts, the first 50
};

// Searches `folder` (recursively) for missing tracks' files: same file name (ignoring case) and same size; when
// several files match, the one with the same content hash. `trackIds` limits it to those tracks (empty = every
// missing track). Updates the paths in one transaction. Blocks: run it on a worker thread.
RelocateResult relocateMissingFiles(Database& db, const std::filesystem::path& folder,
                                    std::span<const std::int64_t> trackIds = {},
                                    const std::function<bool()>& isCancelled = {});

// Marks one track missing / present (e.g. when loading it found no file).
void setTrackMissing(Database& db, std::int64_t trackId, bool missing);

} // namespace devildj::library
