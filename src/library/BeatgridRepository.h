#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace devildj::library
{

class Connection;

// A track's beatgrid as stored (P2-14): beatgrids.anchors_json (positions at the track's sample rate) + locked.
struct StoredBeatgrid
{
    std::string anchorsJson;
    bool locked = false;
    std::optional<int> sampleRate; // tracks.sample_rate: what the positions are in
};

// Reads / writes beatgrids. set() also stores the grid's BPM in tracks.bpm and the lock in tracks.bpm_locked, so
// the analysis service leaves a locked grid and BPM alone. Writes note a Tracks change.
class BeatgridRepository
{
public:
    explicit BeatgridRepository(Connection& connection) noexcept;

    std::optional<StoredBeatgrid> get(std::int64_t trackId);
    bool set(std::int64_t trackId, const std::string& anchorsJson, bool locked, std::optional<double> bpm);
    bool remove(std::int64_t trackId);
    const std::string& error() const noexcept { return lastError; }

private:
    Connection& db;
    std::string lastError;
};

} // namespace devildj::library
