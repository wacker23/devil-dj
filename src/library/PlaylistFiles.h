#pragma once

#include "library/Importer.h"
#include "util/Result.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace devildj::library
{

class Connection;
class Database;

// M3U / M3U8 playlists (P2-06).
struct M3uEntry
{
    std::filesystem::path file;
    std::string title;    // "Artist - Title" from #EXTINF, UTF-8 (may be empty)
    int seconds = -1;     // from #EXTINF; -1 = unknown
};

// Writes an extended M3U8 file: UTF-8 without BOM, "#EXTM3U", one "#EXTINF:<seconds>,<title>" line and the
// absolute path per track, CRLF line ends (what other DJ and music software on Windows expect).
bool writeM3u8(const std::filesystem::path& file, const std::vector<M3uEntry>& entries);

// Reads M3U or M3U8: skips comments and blank lines, keeps #EXTINF titles, resolves relative paths against the
// playlist's folder, accepts file:/// URLs, a UTF-8 BOM and LF or CRLF. .m3u files that aren't valid UTF-8 are
// read in the Windows ANSI code page (old players wrote them that way).
util::Result<std::vector<M3uEntry>> readM3u(const std::filesystem::path& file);

bool isPlaylistFile(const std::filesystem::path& file); // .m3u / .m3u8

// Writes a library playlist (its tracks in order) to an M3U8 file.
util::Result<int> exportPlaylist(Connection& reader, std::int64_t playlistId, const std::filesystem::path& file);

struct PlaylistImportResult
{
    std::int64_t playlistId = 0;
    std::string name;
    int tracks = 0;        // entries in the new playlist
    int addedToLibrary = 0; // files that weren't in the library yet (imported first)
    std::vector<std::string> missing; // files the playlist lists that don't exist (skipped)
};

// Reads an M3U / M3U8 file and makes a library playlist of it, named after the file, inside `parentFolder` (or
// at the top). Files not in the library yet are imported first (importTracks with `options`). Blocks: call it on
// a worker thread.
util::Result<PlaylistImportResult> importPlaylist(Database& db, const std::filesystem::path& file,
                                                  std::optional<std::int64_t> parentFolder = std::nullopt,
                                                  const ImportOptions& options = {});

} // namespace devildj::library
