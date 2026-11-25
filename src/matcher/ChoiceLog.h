#pragma once

// The Track Matcher's choice log (P6-11; MATCHER_SPEC §7.1): what the owner was shown and what they chose, and how
// it went - the personal ranker's training data (P6-12) and the evaluation tool's (P6-13).
//
//   An impression: a track loaded on the idle deck while the other plays: context A, intent, preset, scope, the
//   suggestions shown (track, rank, x) and the chosen B (from the list, or scored on the spot); later its outcome.
//   x(A, B): the 11 features of §4 (null when missing) and 3 interaction terms - energy delta x intent up,
//   energy delta x intent down, sound x intent switch = 14.
//
// Table choice_log (schema v2): shown_json = [{"track": 12, "rank": 1, "chosen": false, "x": [...]}, ...] with the
// chosen track flagged (rank null when it wasn't in the list). Export: CSV, one row per shown track (pandas-friendly,
// long format), or JSON, one object per impression.

#include "matcher/Features.h"
#include "matcher/Scorer.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace devildj::library
{
class Connection;
}

namespace devildj::matcher
{

constexpr int kChoiceDims = kFeatureCount + 3;
using ChoiceVector = std::array<std::optional<double>, kChoiceDims>;
const char* choiceDimName(int i) noexcept; // the features' names, then "energy_up", "energy_down", "sound_switch"

ChoiceVector choiceVector(const std::array<std::optional<double>, kFeatureCount>& features,
                          std::optional<double> energyDelta, Intent intent);

enum class Outcome : int
{
    Unknown = 0,
    Mixed = 1,      // mixed in and played 60 s or more
    Ejected = 2,    // loaded but replaced before that
    ThumbsUp = 3,
    ThumbsDown = 4,
};
const char* outcomeName(Outcome o) noexcept;

struct ShownItem
{
    std::int64_t track = 0;
    std::optional<int> rank; // 1-based; none: not from the list (the chosen track, scored on the spot)
    bool chosen = false;
    ChoiceVector x{};
};

struct Impression
{
    std::int64_t id = 0;
    std::int64_t sessionId = 0, createdAt = 0;
    std::int64_t contextTrack = 0, chosenTrack = 0;
    Intent intent = Intent::Hold;
    std::string preset, scope;
    std::vector<ShownItem> shown; // the list, then the chosen track if it wasn't in it
    std::optional<int> chosenRank;
    Outcome outcome = Outcome::Unknown;
    std::string modelVersion;
};

std::string shownToJson(const std::vector<ShownItem>& shown);
std::vector<ShownItem> shownFromJson(const std::string& json);

class ChoiceLogRepository
{
public:
    explicit ChoiceLogRepository(library::Connection& db) noexcept;
    std::optional<std::int64_t> add(const Impression& impression); // its id
    bool setOutcome(std::int64_t id, Outcome outcome);
    std::vector<Impression> all();
    std::vector<Impression> session(std::int64_t sessionId);
    int count();
    bool clear();

private:
    std::vector<Impression> query(const std::string& where, std::int64_t value);
    library::Connection& db;
};

// Writes the impressions (`std::nullopt` = ok, else the problem).
std::optional<std::string> exportCsv(const std::vector<Impression>& impressions, const std::filesystem::path& file);
std::optional<std::string> exportJson(const std::vector<Impression>& impressions, const std::filesystem::path& file);

} // namespace devildj::matcher
