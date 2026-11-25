#pragma once

#include "matcher/CandidateQuery.h"
#include "matcher/Scorer.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace devildj::library
{
class Database;
}

namespace devildj::matcher
{

// What to rank for: A (the master deck's track) and how (MATCHER_SPEC §2, §3, §6).
struct MatchRequest
{
    std::int64_t contextTrackId = 0;
    bool aInOutro = false; // P6-10: A's playhead is in its mix-out region
    double aBpmNow = 0.0;
    bool keyLockOff = false;
    bool allowKeyShift = false;
    Intent intent = Intent::Hold;
    Scope scope;
    std::string preset = "Balanced";
    std::optional<Weights> weights; // P6-12: the personal ranker's blend (instead of the preset's)
    KeyPolicy keyPolicy = KeyPolicy::Relaxed;
    PlayedPolicy playedPolicy = PlayedPolicy::Exclude;
    double maxStretch = 0.08;
    int minRating = 0;
    std::set<std::int64_t> playedThisSession;
    std::vector<std::string> lastArtists;
    std::int64_t nowUnixSeconds = 0;
};

struct Suggestion
{
    std::int64_t trackId = 0;
    double score = 0.0;
    bool wildcard = false;
    std::vector<std::string> reasons;
    std::array<std::optional<double>, kFeatureCount> features{};
    std::optional<double> energyDelta; // P6-11: for the choice log's interaction terms
    // For the Next Track panel (P3-24): up to 4 chips - key, tempo, energy, then sound / groove / history - each its
    // reason and the feature's value (how good); and every feature as "name  value x weight" lines.
    std::vector<std::pair<std::string, double>> chips;
    std::string breakdown;
};

struct MatchResult
{
    std::int64_t contextTrackId = 0;
    MatchRequest request; // P6-11: what these answer (the choice log records intent, preset, scope)
    std::vector<Suggestion> suggestions;
    int candidates = 0;   // after the SQL prefilter
    double milliseconds = 0.0;
};

// Runs a request on its own worker thread (with its own reader connection) - the latest one only, earlier ones still
// waiting are dropped - and hands the result to onResult on that thread (the app posts it to the message thread).
class MatcherService
{
public:
    explicit MatcherService(library::Database& database, std::vector<Preset> presets = defaultPresets());
    ~MatcherService();
    MatcherService(const MatcherService&) = delete;
    MatcherService& operator=(const MatcherService&) = delete;

    void request(MatchRequest r);
    std::function<void(const MatchResult&)> onResult;
    // Synchronous, on the caller's thread with the caller's connection (tests, the evaluation tool).
    static MatchResult run(library::Connection& db, const MatchRequest& r, const std::vector<Preset>& presets);
    // P6-11: one track scored against the request's context (a chosen track that wasn't in the list); none if A or
    // B isn't analyzed. Features and energy delta only.
    static std::optional<Suggestion> scoreOne(library::Connection& db, const MatchRequest& r, std::int64_t trackId,
                                              const std::vector<Preset>& presets);

private:
    void loop();
    library::Database& database;
    std::vector<Preset> presets;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<MatchRequest> pending;
    bool quit = false;
    std::thread worker;
};

// When to ask again (MATCHER_SPEC §2): the master deck changes, a track loads, A's playhead enters its last 25 %,
// intent / scope / preset change (a settings version), the library's analysis finishes. Pure logic: feed it a
// snapshot now and then; it says whether to run the matcher.
class MatcherTriggers
{
public:
    struct Snapshot
    {
        int masterDeck = -1;
        std::array<std::int64_t, 2> trackIds{};
        double masterFraction = 0.0; // A's playhead / length
        bool masterInOutro = false;  // P6-10: A entered its mix-out region (suggestions change)
        int settingsVersion = 0;
        int analysisVersion = 0; // bumped when an analysis pass finishes
    };
    bool update(const Snapshot& now);

private:
    std::optional<Snapshot> last;
};

} // namespace devildj::matcher
