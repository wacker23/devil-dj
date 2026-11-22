#pragma once

#include "matcher/Features.h"

#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace devildj::matcher
{

// Weighted, explainable scoring (MATCHER_SPEC §3, §5): hard filters, S(A->B) = sum w_i f_i / sum w_i over the features
// that have data, minus penalties; presets; MMR diversity re-rank of the top 50; a wildcard in the last slot.
enum class FeatureId
{
    Tempo,
    Key,
    Energy,
    Sound,
    Groove,
    Vocal,   // Phase 6
    Mix,     // Phase 6
    Tags,
    History,
    Rating,
    Fresh,
    Count
};
constexpr int kFeatureCount = static_cast<int>(FeatureId::Count);
const char* featureName(FeatureId id) noexcept; // "tempo", "key", ... (the presets' JSON names)

struct Weights
{
    std::array<double, kFeatureCount> w{};
    double& operator[](FeatureId id) noexcept { return w[static_cast<std::size_t>(id)]; }
    double operator[](FeatureId id) const noexcept { return w[static_cast<std::size_t>(id)]; }
};

struct Preset
{
    std::string name;
    Weights weights;
};
std::vector<Preset> defaultPresets(); // Balanced (default), Harmonic, Peak time, Warm-up, Open format, Discovery
// matcher/presets.json: [{"name": "...", "weights": {"tempo": 1.0, ...}}, ...]; missing weights from Balanced.
std::string presetsToJson(const std::vector<Preset>& presets);
std::vector<Preset> presetsFromJson(const std::string& json); // the defaults if it can't be read
const Preset* findPreset(const std::vector<Preset>& presets, const std::string& name);

// A track as the matcher sees it.
struct TrackInfo
{
    std::int64_t id = 0;
    std::string contentHash, artist;
    double bpm = 0.0;
    int key = -1;
    std::optional<double> energy, energyIntro, energyOutro;
    std::vector<float> sound;  // library-normalized v1 (32-d), empty if none
    std::vector<float> groove; // 48-d, empty if none
    std::vector<std::string> tags; // genre + custom tags
    int rating = 0;
    int playsLast30Days = 0;
};

enum class KeyPolicy
{
    Strict,  // f_key >= 0.85
    Relaxed, // >= 0.5
    Off,
};
enum class PlayedPolicy
{
    Exclude,
    Penalize,
    Ignore,
};

// What A is and how to choose.
struct Context
{
    TrackInfo a;
    double aBpmNow = 0.0; // A's tempo as heard
    bool keyLockOff = false; // on the target deck: B's key moves with its stretch
    bool allowKeyShift = false;
    Intent intent = Intent::Hold;
    double maxStretch = 0.08;
    KeyPolicy keyPolicy = KeyPolicy::Relaxed;
    PlayedPolicy playedPolicy = PlayedPolicy::Exclude;
    std::set<std::int64_t> playedThisSession;
    std::vector<std::string> lastArtists; // the last 3 tracks' artists
    int minRating = 0;
    // History counts (§4.9): B id -> times played after A; B's artist -> after A's artist; the largest pair count.
    std::unordered_map<std::int64_t, int> pairCounts;
    std::unordered_map<std::string, int> artistPairCounts;
    int maxPairCount = 0;
    int maxPlaysLast30Days = 0;
    // Library calibration of sound similarity (§4.4).
    double c10 = 0.0, c90 = 1.0;
    HarmonicTable harmonic;
};

struct Scored
{
    const TrackInfo* track = nullptr;
    double score = 0.0;
    double penalty = 0.0;
    std::array<Feature, kFeatureCount> features{};
    bool wildcard = false;
    // The strongest reasons, best first (up to 4).
    std::vector<std::string> reasons;
};

// §3 in memory (after the SQL prefilter): tempo reachable, key policy, not A, not a duplicate of A, session policy,
// rating.
bool passesFilters(const Context& context, const TrackInfo& b);
// §4-5 for one candidate (no filters).
Scored score(const Context& context, const TrackInfo& b, const Weights& weights);
// Everything: filters, scores, the top 50 re-ranked by MMR (lambda 0.8, sound cosine, no near-duplicates when there
// is a choice), the wildcard last. Returns up to `count` (10).
std::vector<Scored> rank(const Context& context, const std::vector<TrackInfo>& candidates, const Weights& weights,
                         int count = 10);

// Library percentiles of pairwise sound similarity (the 10th and 90th) from a deterministic sample of pairs.
std::pair<double, double> soundPercentiles(const std::vector<TrackInfo>& tracks, int samplePairs = 2000);

} // namespace devildj::matcher
