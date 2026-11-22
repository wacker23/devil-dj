// P3-19: Track Matcher v1 core. Done when: all MATCHER_SPEC §13 tests for layer 1 pass; 5,000 candidates scored in
// <= 30 ms (Release).

#include "analysis/TrackFeatures.h"
#include "library/Connection.h"
#include "library/Database.h"
#include "matcher/Camelot.h"
#include "matcher/CandidateQuery.h"
#include "matcher/Features.h"
#include "matcher/MatcherService.h"
#include "matcher/Scorer.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <thread>

using namespace devildj;
using namespace devildj::matcher;
namespace fs = std::filesystem;

namespace
{
int major(int pitch) { return pitch; }
int minor(int pitch) { return 12 + pitch; }

std::vector<float> randomVector(std::mt19937& rng, int n)
{
    std::normal_distribution<float> d(0.0f, 1.0f);
    std::vector<float> v(static_cast<std::size_t>(n));
    for (auto& x : v)
        x = d(rng);
    return v;
}

TrackInfo track(std::int64_t id, double bpm, int key, double energy, std::vector<float> sound = {})
{
    TrackInfo t;
    t.id = id;
    t.contentHash = "h" + std::to_string(id);
    t.artist = "Artist " + std::to_string(id);
    t.bpm = bpm;
    t.key = key;
    t.energy = energy;
    t.sound = std::move(sound);
    return t;
}
} // namespace

TEST_CASE("Matcher Camelot: all 24 keys; the relationship table and its symmetry", "[matcher]")
{
    // C major 8B, A minor 8A, G major 9B, E minor 9A, F major 7B, D minor 7A, and round the wheel.
    CHECK(camelotCode(major(0)) == "8B");
    CHECK(camelotCode(minor(9)) == "8A");
    CHECK(camelotCode(major(7)) == "9B");
    CHECK(camelotCode(minor(4)) == "9A");
    CHECK(camelotCode(major(5)) == "7B");
    CHECK(camelotCode(minor(2)) == "7A");
    std::set<std::string> codes;
    for (int k = 0; k < 24; ++k)
        codes.insert(camelotCode(k));
    CHECK(codes.size() == 24); // each key its own code
    for (int k = 0; k < 12; ++k) // a fifth up is +1 on the wheel, a relative minor the same number
    {
        CHECK(camelotNumber(major((k + 7) % 12)) == camelotNumber(major(k)) % 12 + 1);
        CHECK(camelotNumber(minor((k + 9) % 12)) == camelotNumber(major(k)));
    }
    const HarmonicTable table;
    CHECK(relation(major(0), major(0)) == KeyRelation::Same);
    CHECK(relation(major(0), major(7)) == KeyRelation::Adjacent);    // 8B -> 9B
    CHECK(relation(major(0), major(5)) == KeyRelation::Adjacent);    // 8B -> 7B
    CHECK(relation(major(0), minor(9)) == KeyRelation::Relative);    // 8B -> 8A
    CHECK(relation(major(0), minor(4)) == KeyRelation::Diagonal);    // 8B -> 9A
    CHECK(relation(major(0), major(2)) == KeyRelation::EnergyBoost); // 8B -> 10B
    CHECK(relation(major(0), major(1)) == KeyRelation::SemitoneUp);  // 8B -> 3B (+7 = -5)
    CHECK(relation(major(0), major(10)) == KeyRelation::TwoDown);    // 8B -> 6B
    CHECK(relation(major(0), major(11)) == KeyRelation::SemitoneDown); // 8B -> 1B (-7 = +5)
    CHECK(relation(major(0), minor(1)) == KeyRelation::Other);
    // Same / adjacent / relative / diagonal are symmetric; the energy boost isn't (its reverse is "two down").
    for (int a = 0; a < 24; ++a)
        for (int b = 0; b < 24; ++b)
        {
            const auto r = relation(a, b);
            if (r == KeyRelation::Same || r == KeyRelation::Adjacent || r == KeyRelation::Relative ||
                r == KeyRelation::Diagonal)
                CHECK(relation(b, a) == r);
            if (r == KeyRelation::EnergyBoost)
                CHECK(relation(b, a) == KeyRelation::TwoDown);
            if (r == KeyRelation::SemitoneUp)
                CHECK(relation(b, a) == KeyRelation::SemitoneDown);
        }
    CHECK(table.of(KeyRelation::Same) == 1.0);
    CHECK(table.of(KeyRelation::Other) == 0.10);
    HarmonicTable tuned;
    REQUIRE(tuned.load(R"({"adjacent": 0.8, "other": 0.05})"));
    CHECK(tuned.of(KeyRelation::Adjacent) == 0.8);
    CHECK(tuned.of(KeyRelation::Other) == 0.05);
    CHECK(tuned.of(KeyRelation::Relative) == 0.85);
    CHECK_FALSE(tuned.load("nonsense"));
}

TEST_CASE("Matcher features: boundaries, missing data, reasons", "[matcher]")
{
    // Tempo.
    double s = 0.0, m = 0.0;
    auto t = tempoFit(128.0, 128.0, &s, &m);
    CHECK(*t.value == 1.0);
    CHECK(t.reason == "BPM +0.0 %");
    t = tempoFit(124.0, 62.5, &s, &m);
    CHECK(m == 2.0);
    CHECK(t.reason == "BPM x2");
    t = tempoFit(128.0, 125.0);
    CHECK(std::abs(*t.value - std::exp(-std::pow(0.024 / 0.03, 2.0))) < 1e-9);
    CHECK(t.reason == "BPM +2.4 %");
    CHECK_FALSE(tempoFit(0.0, 120.0).value);
    CHECK(tempoReachable(128.0, 120.0));        // 6.7 %
    CHECK_FALSE(tempoReachable(128.0, 115.0));  // 11 %
    CHECK(tempoReachable(174.0, 87.0));         // half time
    // Key: with key lock off a 6 % stretch moves B a semitone up (12 log2 1.06 = 1.0).
    const HarmonicTable table;
    auto k = keyFit(minor(9), minor(9), table);
    CHECK(*k.value == 1.0);
    CHECK(k.reason == "8A -> 8A");
    k = keyFit(major(0), major(11), table, 0.06, true); // B major + 1 semitone = C major
    CHECK(*k.value == 1.0);
    k = keyFit(major(0), major(1), table, 0.0, false, true); // Db: a semitone down is C (shift -1)
    CHECK(std::abs(*k.value - 0.9) < 1e-9);
    CHECK(k.reason == "8B -> 8B (shift -1)");
    CHECK_FALSE(keyFit(-1, 3, table).value);
    // Energy.
    auto e = energyFit(0.5, 0.6, Intent::Up);
    CHECK(*e.value == 1.0);
    CHECK(e.reason == "Energy 5 -> 6 up");
    CHECK(*energyFit(0.5, 0.5, Intent::Hold).value == 1.0);
    CHECK(*energyFit(0.5, 0.8, Intent::Switch).value > *energyFit(0.5, 0.8, Intent::Hold).value);
    CHECK_FALSE(energyFit(std::nullopt, 0.5, Intent::Hold).value);
    CHECK(energyLevel(0.0) == 1);
    CHECK(energyLevel(1.0) == 10);
    // Sound: calibrated by the library's percentiles; inverted for switch; missing.
    const std::vector<float> a{1, 0, 0}, b{1, 0, 0}, c{0, 1, 0};
    CHECK(*soundFit(a, b, 0.0, 0.5, Intent::Hold).value == 1.0);
    CHECK(*soundFit(a, c, 0.0, 0.5, Intent::Hold).value == 0.0);
    CHECK(*soundFit(a, c, 0.0, 0.5, Intent::Switch).value == 1.0);
    CHECK(soundFit(a, b, 0.0, 0.5, Intent::Hold).reason == "Similar sound 100 %");
    CHECK_FALSE(soundFit({}, b, 0.0, 1.0, Intent::Hold).value);
    // Groove, tags, history, rating, freshness.
    CHECK(grooveFit(a, b).reason == "Same groove");
    CHECK(*grooveFit(a, c).value == 0.0);
    CHECK(std::abs(*tagsFit({"House", "Deep"}, {"house", "Tech"}).value - 1.0 / 3.0) < 1e-9);
    CHECK_FALSE(tagsFit({}, {"x"}).value);
    auto h = historyFit(3, 0, 0, 3);
    CHECK(std::abs(*h.value - 0.6) < 1e-9);
    CHECK(h.reason == "You played this after A 3x");
    CHECK(*historyFit(0, 0, 0, 0).value == 0.0);
    CHECK(*ratingFit(0).value == 0.5);
    CHECK(*ratingFit(4).value == 0.8);
    CHECK(*freshnessFit(5, 10).value == 0.5);
    CHECK(*freshnessFit(0, 0).value == 1.0);
}

TEST_CASE("Matcher scorer: renormalization when data is missing, presets, penalties, filters", "[matcher]")
{
    Context c;
    c.a = track(1, 128.0, minor(9), 0.5);
    c.aBpmNow = 128.0;
    Weights w = defaultPresets().front().weights;
    // B with only tempo, key, energy, rating, freshness, history (no sound / groove / tags): the mean is over those.
    const auto b = track(2, 128.0, minor(9), 0.5);
    const auto s = score(c, b, w);
    const double expected = (1.0 * 1.0 + 1.0 * 1.0 + 1.0 * 1.0 + 0.5 * 0.0 + 0.3 * 0.5 + 0.2 * 1.0) /
                            (1.0 + 1.0 + 1.0 + 0.5 + 0.3 + 0.2);
    CHECK(std::abs(s.score - expected) < 1e-9);
    CHECK_FALSE(s.features[static_cast<std::size_t>(FeatureId::Sound)].value);
    CHECK_FALSE(s.reasons.empty());
    // Penalties: the same artist as one just played, and played this session (penalize).
    auto same = b;
    same.artist = "DJ X";
    c.lastArtists = {"dj x"};
    CHECK(std::abs(score(c, same, w).score - (expected - 0.15)) < 1e-9);
    c.lastArtists.clear();
    c.playedPolicy = PlayedPolicy::Penalize;
    c.playedThisSession = {2};
    CHECK(std::abs(score(c, b, w).score - (expected - 0.30)) < 1e-9);
    // Filters: excluded when played, A itself, its duplicate, too far in tempo, the wrong key for the policy.
    c.playedPolicy = PlayedPolicy::Exclude;
    CHECK_FALSE(passesFilters(c, b));
    c.playedThisSession.clear();
    CHECK(passesFilters(c, b));
    CHECK_FALSE(passesFilters(c, c.a));
    auto duplicate = b;
    duplicate.contentHash = c.a.contentHash;
    CHECK_FALSE(passesFilters(c, duplicate));
    CHECK_FALSE(passesFilters(c, track(3, 110.0, minor(9), 0.5)));
    c.keyPolicy = KeyPolicy::Strict;
    CHECK_FALSE(passesFilters(c, track(4, 128.0, minor(4), 0.5)) == false); // 8A -> 9A adjacent 0.9: passes
    CHECK_FALSE(passesFilters(c, track(5, 128.0, major(7), 0.5)));          // 8A -> 9B diagonal 0.6: strict says no
    c.keyPolicy = KeyPolicy::Relaxed;
    CHECK(passesFilters(c, track(5, 128.0, major(7), 0.5)));
    // Presets: six, Balanced first; JSON round trip; a bad file gives the defaults.
    const auto presets = defaultPresets();
    REQUIRE(presets.size() == 6);
    CHECK(presets[1].name == "Harmonic");
    CHECK(presets[1].weights[FeatureId::Key] == 2.0);
    const auto back = presetsFromJson(presetsToJson(presets));
    REQUIRE(back.size() == 6);
    for (std::size_t p = 0; p < presets.size(); ++p)
    {
        CHECK(back[p].name == presets[p].name);
        CHECK(back[p].weights.w == presets[p].weights.w);
    }
    CHECK(presetsFromJson("garbage").size() == 6);
    CHECK(findPreset(presets, "Peak time")->weights[FeatureId::Energy] == 1.6);
}

TEST_CASE("Matcher MMR: no near-duplicates in the top 10 when there are alternatives; the wildcard", "[matcher]")
{
    std::mt19937 rng(7);
    Context c;
    const auto base = randomVector(rng, 32);
    c.a = track(1, 126.0, minor(9), 0.5, base);
    c.aBpmNow = 126.0;
    std::vector<TrackInfo> candidates;
    // 15 near-copies of one sound (cosine > 0.99 between them), all scoring well...
    const auto clone = randomVector(rng, 32);
    for (int i = 0; i < 15; ++i)
    {
        auto v = clone;
        for (auto& x : v)
            x += 0.01f * std::normal_distribution<float>(0.0f, 1.0f)(rng);
        candidates.push_back(track(100 + i, 126.0, minor(9), 0.5, v));
    }
    // ...and 30 others, a little further in key / tempo, each its own sound.
    for (int i = 0; i < 30; ++i)
        candidates.push_back(track(200 + i, 124.0 + (i % 4), i % 2 == 0 ? minor(4) : minor(9), 0.5 + 0.01 * (i % 5),
                                   randomVector(rng, 32)));
    const auto [c10, c90] = soundPercentiles(candidates);
    c.c10 = c10;
    c.c90 = c90;
    const auto top = rank(c, candidates, defaultPresets().front().weights, 10);
    REQUIRE(top.size() == 10);
    for (std::size_t i = 0; i < top.size(); ++i)
        for (std::size_t j = i + 1; j < top.size(); ++j)
            CHECK(cosine(top[i].track->sound, top[j].track->sound) <= 0.98);
    CHECK(top.back().wildcard);
    for (std::size_t i = 0; i + 1 < top.size(); ++i)
        CHECK_FALSE(top[i].wildcard);
}

TEST_CASE("Matcher in the library: the SQL prefilter, history counts, the service; 5,000 candidates in <= 30 ms",
          "[matcher][library]")
{
    const auto dir = fs::temp_directory_path() /
                     ("devildj-matcher-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::mt19937 rng(11);
    constexpr int n = 5000;
    {
        auto db = std::move(library::Database::open(dir / "library.db")).value();
        db->writeAndWait(
            [&](library::Connection& conn)
            {
                conn.exec("BEGIN");
                auto* ins = conn.prepare("INSERT INTO tracks (id, path, content_hash, artist, genre, bpm, musical_key, "
                                         "rating, date_added, missing) VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, ?)");
                auto* feat = conn.prepare("INSERT INTO track_features (track_id, version, energy, sound_v1, groove) "
                                          "VALUES (?, 1, ?, ?, ?)");
                for (int i = 1; i <= n + 3; ++i)
                {
                    const bool unanalyzed = i == n + 1, missing = i == n + 2;
                    ins->bind(1, static_cast<std::int64_t>(i)).bind(2, "C:\\m\\" + std::to_string(i) + ".mp3")
                        .bind(3, "hash" + std::to_string(i)).bind(4, "Artist " + std::to_string(i % 97))
                        .bind(5, i % 3 == 0 ? "House" : "Techno");
                    if (unanalyzed)
                        ins->bindNull(6);
                    else
                        ins->bind(6, 118.0 + (i % 20));
                    ins->bind(7, i % 24).bind(8, i % 6).bind(9, missing ? 1 : 0);
                    REQUIRE(ins->run());
                    ins->reset();
                    const auto sound = analysis::packFloats(randomVector(rng, 32));
                    auto groove = randomVector(rng, 48);
                    for (auto& x : groove)
                        x = std::abs(x);
                    const auto grooveBlob = analysis::packFloats(groove);
                    feat->bind(1, static_cast<std::int64_t>(i)).bind(2, 0.3 + 0.4 * ((i % 10) / 10.0))
                        .bind(3, std::span<const std::uint8_t>(sound)).bind(4, std::span<const std::uint8_t>(grooveBlob));
                    REQUIRE(feat->run());
                    feat->reset();
                }
                // History: 7 after 1 three times, 9 after 1 once.
                for (const auto [prev, next] : {std::pair{1, 7}, std::pair{1, 7}, std::pair{1, 7}, std::pair{1, 9}})
                    conn.exec("INSERT INTO history (session_id, track_id, played_at, previous_track_id) VALUES (1, " +
                                 std::to_string(next) + ", 1000, " + std::to_string(prev) + ")");
                conn.exec("COMMIT");
            });
    }
    auto db = std::move(library::Database::open(dir / "library.db")).value();
    auto reader = std::move(db->openReader()).value();
    const auto candidates = loadCandidates(*reader, {}, 1, 0, 2000);
    CHECK(candidates.size() == static_cast<std::size_t>(n)); // n + 3 tracks, minus A, the unanalyzed and the missing
    for (const auto& t : candidates)
    {
        REQUIRE(t.id != n + 1);
        REQUIRE(t.id != n + 2);
    }
    CHECK(candidates.front().sound.size() == 32);
    CHECK(candidates.front().groove.size() == 48);
    Context c;
    c.a = *loadTrack(*reader, 1, 2000);
    loadHistory(*reader, c, 2000);
    CHECK(c.pairCounts[7] == 3);
    CHECK(c.pairCounts[9] == 1);
    CHECK(c.maxPairCount == 3);

    // Scoring 5,000: the in-memory part (what the budget is for), timed over a few runs.
    c.aBpmNow = c.a.bpm;
    c.keyPolicy = KeyPolicy::Off;
    c.maxStretch = 0.5;
    const auto [c10, c90] = soundPercentiles(candidates);
    c.c10 = c10;
    c.c90 = c90;
    double best = 1e9;
    std::vector<Scored> top;
    for (int run = 0; run < 5; ++run)
    {
        const auto t0 = std::chrono::steady_clock::now();
        top = rank(c, candidates, defaultPresets().front().weights, 10);
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    WARN("5,000 candidates scored and re-ranked in " << best << " ms");
    REQUIRE(top.size() == 10);
#ifdef NDEBUG
    CHECK(best <= 30.0);
#endif
    // The service end to end (its own thread and connection).
    MatcherService service(*db);
    std::atomic<bool> done{false};
    MatchResult result;
    service.onResult = [&](const MatchResult& r)
    {
        result = r;
        done = true;
    };
    MatchRequest request;
    request.contextTrackId = 1;
    request.keyPolicy = KeyPolicy::Off;
    request.nowUnixSeconds = 2000;
    service.request(request);
    for (int i = 0; i < 500 && !done; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(done);
    CHECK(result.suggestions.size() == 10);
    CHECK(result.candidates > 1000);
    WARN("matcher request (SQL + scoring): " << result.milliseconds << " ms for " << result.candidates << " candidates");
    reader.reset();
    db.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Matcher triggers: master change, a load, A into its last quarter, settings, analysis", "[matcher]")
{
    MatcherTriggers t;
    MatcherTriggers::Snapshot s;
    CHECK_FALSE(t.update(s)); // no master deck: nothing to rank for
    s.masterDeck = 0;
    s.trackIds = {10, 0};
    CHECK(t.update(s));       // a master now
    CHECK_FALSE(t.update(s)); // the same
    s.trackIds = {10, 20};
    CHECK(t.update(s)); // a track loaded
    s.masterFraction = 0.5;
    CHECK_FALSE(t.update(s));
    s.masterFraction = 0.76;
    CHECK(t.update(s)); // into the last 25 %
    s.masterFraction = 0.8;
    CHECK_FALSE(t.update(s));
    s.settingsVersion = 1;
    CHECK(t.update(s));
    s.analysisVersion = 4;
    CHECK(t.update(s));
    s.masterDeck = 1;
    CHECK(t.update(s));
}
