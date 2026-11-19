#pragma once

#include "util/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace devildj::library
{

class Connection;

// One row of `tracks` (ARCHITECTURE §7). Text is UTF-8; empty text is stored as NULL.
struct Track
{
    std::int64_t id = 0;
    std::string path, contentHash;
    std::string title, artist, album, genre, comment;
    std::optional<int> year;
    std::optional<std::int64_t> durationMs;
    std::optional<int> sampleRate, bitrateKbps;
    std::optional<std::int64_t> fileSize;
    std::optional<double> bpm;
    bool bpmLocked = false;
    std::optional<int> musicalKey; // 0..23 (TagReader.h parseKeyTag)
    std::optional<double> keyConfidence;
    std::optional<double> loudnessLufs, autoGainDb;
    int rating = 0;                   // 0..5 stars
    std::optional<std::uint32_t> color; // 0xRRGGBB
    int playCount = 0;
    std::optional<std::int64_t> lastPlayed; // unix seconds
    std::int64_t dateAdded = 0;             // unix seconds
    int analysisVersion = 0;                // 0 = not analyzed yet
    bool missing = false;

    friend bool operator==(const Track&, const Track&) = default;
};

// Tracks in the library (P2-03). Works on the connection it is given: a reader connection for the get / list
// calls, the writer connection (inside a Database::write job) for add / update / remove, which note their
// changes for the change listeners. Errors are returned, never thrown; error() says why the last call failed.
class TrackRepository
{
public:
    explicit TrackRepository(Connection& connection) noexcept;

    util::Result<std::int64_t> add(const Track& track); // id is ignored; dateAdded 0 = now
    std::optional<Track> get(std::int64_t id);
    std::optional<Track> findByPath(const std::string& path); // exact stored path (Importer.h libraryPath)
    std::vector<Track> list(int offset = 0, int limit = -1);  // by id
    std::int64_t count();
    bool update(const Track& track); // every column but id
    bool remove(std::int64_t id);    // also its cues, beatgrid, tags and playlist entries

    // One-field edits from the browser (P2-08), so a click doesn't rewrite the whole row.
    bool setRating(std::int64_t id, int stars);                         // 0..5
    bool setColour(std::int64_t id, std::optional<std::uint32_t> rgb); // nullopt = no colour

    const std::string& error() const noexcept { return lastError; }

private:
    bool fail(std::string message);
    Connection& db;
    std::string lastError;
};

} // namespace devildj::library
