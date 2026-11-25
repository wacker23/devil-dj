#pragma once

// The Track Matcher's offline evaluation (P6-13; MATCHER_SPEC §10), used by tools/matcher_eval.
//
//   Offline replay: every transition the owner actually played (history: A then B), B ranked among the library as
//   it was then - tracks added later left out, the history and play counts only from before it (no hindsight) -
//   with the default filters (tempo reachable, key policy relaxed). Rank 1 = B scored highest; B filtered out =
//   a miss. HitRate@1/5/10 and MRR per weight set: every preset, the personal ranker's blend, and Balanced with
//   one feature left out at a time (ablation). Each candidate's features are computed once per transition; the
//   weight sets only re-weigh them.
//   Tuning: coordinate ascent on Balanced's weights for MRR (a suggestion to retune the presets from; in-sample).
//   The choice log's product metrics: share of loads taken from the list, mean rank of the chosen, thumbs.
//
// Smart Sort orderings (§10's third point) come with Smart Sort itself (Phase 7).

#include "matcher/Ranker.h"
#include "matcher/Scorer.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace devildj::library
{
class Connection;
}

namespace devildj::matcher
{

struct EvalTransition
{
    std::int64_t a = 0, b = 0;
    std::int64_t at = 0; // played_at of B
    std::int64_t session = 0;
};

struct EvalData
{
    std::vector<TrackInfo> tracks;                           // every analyzed track (features as now)
    std::unordered_map<std::int64_t, std::size_t> index;     // id -> tracks
    std::unordered_map<std::int64_t, std::int64_t> dateAdded;
    struct Play
    {
        std::int64_t track = 0, previous = 0, at = 0;
    };
    std::vector<Play> history; // in time order
    std::vector<EvalTransition> transitions;
    double c10 = 0.0, c90 = 1.0;
};
EvalData loadEvalData(library::Connection& db);

// One transition's candidates as the matcher saw them then: each one's features; which is B (-1: B filtered out).
struct ReplayCase
{
    std::vector<std::array<std::optional<double>, kFeatureCount>> features;
    int b = -1;
};
ReplayCase replayCase(const EvalData& data, const EvalTransition& t, KeyPolicy keyPolicy = KeyPolicy::Relaxed);

// The weighted score as the scorer makes it (intent hold, no penalties in the replay).
double weightedScore(const std::array<std::optional<double>, kFeatureCount>& f, const Weights& w) noexcept;
int rankOfB(const ReplayCase& c, const Weights& w); // 1-based; 0 = a miss

struct EvalMetrics
{
    int n = 0, hit1 = 0, hit5 = 0, hit10 = 0, misses = 0;
    double reciprocal = 0.0;
    void add(int rank);
    double hitRate(int k) const noexcept;
    double mrr() const noexcept { return n > 0 ? reciprocal / n : 0.0; }
};
EvalMetrics evaluate(const std::vector<ReplayCase>& cases, const Weights& w);

struct WeightSet
{
    std::string name;
    Weights weights;
};
// Every preset, the personal ranker's blend (when given), and "Balanced - <feature>" for each feature.
std::vector<WeightSet> weightSets(const std::vector<Preset>& presets, const std::optional<PersonalModel>& personal);

// Coordinate ascent: each weight times 0 / 0.5 / 1 / 1.5 / 2 / 3, kept when MRR improves; `passes` rounds.
Weights tuneWeights(const std::vector<ReplayCase>& cases, const Weights& start, int passes = 2);

struct ChoiceStats
{
    int impressions = 0, fromList = 0, thumbsUp = 0, thumbsDown = 0, mixed = 0, ejected = 0;
    double meanChosenRank = 0.0; // of those from the list
};
ChoiceStats choiceStats(library::Connection& db);

struct EvalReport
{
    std::string date;
    int tracks = 0, transitions = 0, sessions = 0;
    std::vector<std::pair<std::string, EvalMetrics>> results; // in weightSets order
    std::optional<Weights> tuned;
    EvalMetrics tunedMetrics;
    ChoiceStats choices;
    std::optional<PersonalModel> personal;
};
std::string reportMarkdown(const EvalReport& report);

} // namespace devildj::matcher
