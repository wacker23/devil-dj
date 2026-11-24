#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace devildj::library
{

class Connection;

// The owner's answers to smart cue suggestions (P6-07): accepted (it became a hot cue) or rejected.
struct CueAnswer
{
    std::int64_t trackId = 0;
    double startSeconds = 0.0;
    std::string reason; // "DROP", "BREAK", ...
    bool accepted = false;
    std::int64_t at = 0; // unix seconds
};

class CueFeedbackRepository
{
public:
    explicit CueFeedbackRepository(Connection& connection) noexcept;

    bool add(const CueAnswer& answer);
    std::vector<CueAnswer> forTrack(std::int64_t trackId);
    std::vector<CueAnswer> all();
    int count(bool accepted);
    const std::string& error() const noexcept { return lastError; }

private:
    std::vector<CueAnswer> query(const std::string& where, std::int64_t trackId);
    Connection& db;
    std::string lastError;
};

} // namespace devildj::library
