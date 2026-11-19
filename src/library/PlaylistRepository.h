#pragma once

#include "util/Result.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace devildj::library
{

class Connection;

enum class PlaylistKind
{
    Folder = 0,
    Playlist = 1,
    Smart = 2,   // rules in ruleJson (P2-07); its tracks are computed, not stored
    History = 3, // P2-23
};

// One row of `playlists`. Folders hold playlists and folders; the other kinds hold tracks.
struct Playlist
{
    std::int64_t id = 0;
    std::optional<std::int64_t> parentId; // a folder, or nullopt for the top level
    std::string name;
    PlaylistKind kind = PlaylistKind::Playlist;
    std::string ruleJson;
    int sortOrder = 0;
    std::int64_t createdAt = 0; // unix seconds

    friend bool operator==(const Playlist&, const Playlist&) = default;
};

// Playlists, folders and their track lists (P2-03). Same rules as TrackRepository: reads on any connection,
// writes on the writer connection (they note PlaylistTracks / Playlists changes), errors via error().
class PlaylistRepository
{
public:
    explicit PlaylistRepository(Connection& connection) noexcept;

    // name is trimmed and must not be empty; parent must be a folder; createdAt 0 = now; sortOrder 0 = after
    // its siblings.
    util::Result<std::int64_t> create(const Playlist& playlist);
    std::optional<Playlist> get(std::int64_t id);
    std::vector<Playlist> children(std::optional<std::int64_t> parentId); // by sortOrder, then name
    std::vector<Playlist> all();                                          // by id
    bool update(const Playlist& playlist); // rename, move to another folder, reorder (not kind changes)
    bool remove(std::int64_t id);          // with everything inside a folder

    // Track lists (not folders, not smart playlists). Positions are 0-based; the same track may appear twice.
    std::vector<std::int64_t> trackIds(std::int64_t playlistId);
    bool setTracks(std::int64_t playlistId, std::span<const std::int64_t> trackIds);
    bool addTracks(std::int64_t playlistId, std::span<const std::int64_t> trackIds, int insertAt = -1); // -1: end
    bool removeTracksAt(std::int64_t playlistId, std::span<const int> positions);
    bool moveTrack(std::int64_t playlistId, int from, int to);

    const std::string& error() const noexcept { return lastError; }

private:
    bool fail(std::string message);
    bool holdsTracks(std::int64_t playlistId);
    bool isInside(std::int64_t folder, std::int64_t playlistId); // folder is playlistId or one of its descendants
    Connection& db;
    std::string lastError;
};

} // namespace devildj::library
