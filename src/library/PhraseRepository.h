#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace devildj::library
{

class Connection;

// A track's phrases (P6-06): where each starts (seconds from the start of the audio), how many bars, and its
// energy (0..1). `edited`: the owner changed them - analysis then leaves them alone.
struct StoredPhrase
{
    double startSeconds = 0.0;
    int bars = 0;
    double energy = 0.0;
    friend bool operator==(const StoredPhrase&, const StoredPhrase&) = default;
};

struct StoredPhrases
{
    std::vector<StoredPhrase> phrases; // by start
    bool edited = false;
};

// Reads / writes `phrases`. set() replaces the track's rows. setFromAnalysis() does the same only when the owner
// hasn't edited them (returns false and leaves them otherwise). Writes note a Tracks change.
class PhraseRepository
{
public:
    explicit PhraseRepository(Connection& connection) noexcept;

    StoredPhrases get(std::int64_t trackId);
    bool set(std::int64_t trackId, const std::vector<StoredPhrase>& phrases, bool edited);
    bool setFromAnalysis(std::int64_t trackId, const std::vector<StoredPhrase>& phrases);
    bool edited(std::int64_t trackId);
    // Tracks whose phrases the owner edited (the evaluation's ground truth).
    std::vector<std::int64_t> editedTracks();
    const std::string& error() const noexcept { return lastError; }

private:
    bool write(std::int64_t trackId, const std::vector<StoredPhrase>& phrases, bool edited);
    Connection& db;
    std::string lastError;
};

} // namespace devildj::library
