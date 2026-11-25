// P6-13: the matcher's offline evaluation - ranks, metrics, nothing seen from the future, the weight sets, tuning,
// the report.

#include "library/Connection.h"
#include "library/Database.h"
#include "library/TrackRepository.h"
#include "matcher/Evaluation.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>

using namespace devildj;
using namespace devildj::matcher;

namespace
{
std::array<std::optional<double>, kFeatureCount> only(FeatureId id, double v)
{
    std::array<std::optional<double>, kFeatureCount> f{};
    for (auto& x : f)
        x = 0.5;
    f[static_cast<std::size_t>(id)] = v;
    return f;
}

struct Lib
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                ("devildj-eval-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::unique_ptr<library::Database> db = std::move(library::Database::open(dir / "library.db")).value();
    ~Lib()
    {
        db.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};
} // namespace

TEST_CASE("Evaluation: ranks, misses, HitRate and MRR", "[matcher][eval]")
{
    ReplayCase c;
    c.features = {only(FeatureId::Key, 0.2), only(FeatureId::Key, 0.9), only(FeatureId::Key, 0.5)};
    c.b = 2; // B is the middle one by key
    auto w = defaultPresets().front().weights;
    CHECK(rankOfB(c, w) == 2);
    w[FeatureId::Key] = 0.0; // key ignored: all tied, B in the middle of the tie
    CHECK(rankOfB(c, w) == 2);
    ReplayCase miss;
    CHECK(rankOfB(miss, w) == 0);

    EvalMetrics m;
    for (const int r : {1, 2, 7, 0})
        m.add(r);
    CHECK(m.n == 4);
    CHECK(m.misses == 1);
    CHECK(m.hitRate(1) == 0.25);
    CHECK(m.hitRate(5) == 0.5);
    CHECK(m.hitRate(10) == 0.75);
    CHECK(std::abs(m.mrr() - (1.0 + 0.5 + 1.0 / 7.0) / 4.0) < 1e-12);
}

TEST_CASE("Evaluation: the weight sets, ablation and tuning", "[matcher][eval]")
{
    std::optional<PersonalModel> personal = PersonalModel{};
    personal->impressions = 200;
    personal->weights = rankWeightsOf(defaultPresets().front().weights);
    const auto sets = weightSets(defaultPresets(), personal);
    CHECK(sets.size() == defaultPresets().size() + 1 + kFeatureCount);
    CHECK(sets[defaultPresets().size()].name.rfind("Personal", 0) == 0);
    CHECK(sets.back().name == "Balanced - fresh");
    CHECK(sets.back().weights[FeatureId::Fresh] == 0.0);

    // An owner who follows groove only: tuning raises groove (and MRR) over Balanced.
    std::vector<ReplayCase> cases;
    for (int i = 0; i < 20; ++i)
    {
        ReplayCase c;
        for (int k = 0; k < 8; ++k)
        {
            auto f = only(FeatureId::Groove, 0.1 * k);
            f[static_cast<std::size_t>(FeatureId::Key)] = 1.0 - 0.1 * k; // key points the other way
            c.features.push_back(f);
        }
        c.b = 7; // the best groove, the worst key
        cases.push_back(c);
    }
    const auto balanced = defaultPresets().front().weights;
    const auto before = evaluate(cases, balanced).mrr();
    const auto tuned = tuneWeights(cases, balanced);
    const auto after = evaluate(cases, tuned).mrr();
    INFO("MRR " << before << " -> " << after);
    CHECK(after > before);
    CHECK(after == 1.0);
    CHECK(tuned[FeatureId::Groove] / tuned[FeatureId::Key] > balanced[FeatureId::Groove] / balanced[FeatureId::Key]);
}

TEST_CASE("Evaluation: the replay uses only what was known then", "[matcher][eval][library]")
{
    Lib lib;
    std::int64_t a = 0, b = 0, c = 0, later = 0;
    lib.db->writeAndWait(
        [&](library::Connection& conn)
        {
            library::TrackRepository tracks(conn);
            const auto add = [&](const char* name, int key, std::int64_t added)
            {
                library::Track t;
                t.path = std::string("C:\\x\\") + name + ".wav";
                t.contentHash = name;
                t.bpm = 124.0;
                t.musicalKey = key;
                t.dateAdded = added;
                return tracks.add(t).value();
            };
            a = add("a", 21, 100);
            b = add("b", 21, 100);
            c = add("c", 21, 100);
            later = add("later", 21, 5000); // added after the transition
            // Session 1 at t=1000: a then b, then c then b. Session 2 at t=3000: a then b again.
            REQUIRE(conn.exec("INSERT INTO history (session_id, track_id, played_at, deck, previous_track_id) VALUES "
                              "(1, " + std::to_string(a) + ", 1000, 0, NULL), (1, " + std::to_string(b) + ", 1100, 1, " +
                              std::to_string(a) + "), (1, " + std::to_string(c) + ", 1200, 0, " + std::to_string(b) +
                              "), (2, " + std::to_string(a) + ", 3000, 0, NULL), (2, " + std::to_string(b) + ", 3100, 1, " +
                              std::to_string(a) + ")"));
        });
    auto reader = std::move(lib.db->openReader()).value();
    const auto data = loadEvalData(*reader);
    REQUIRE(data.transitions.size() == 3);
    CHECK(data.transitions[0].a == a);
    CHECK(data.transitions[0].b == b);
    CHECK(data.tracks.size() == 4);

    // The first a -> b: b had never followed a (history 0); "later" wasn't in the library yet.
    const auto first = replayCase(data, data.transitions[0]);
    REQUIRE(first.b >= 0);
    CHECK(first.features.size() == 2); // b and c (not a itself, not "later")
    CHECK(first.features[static_cast<std::size_t>(first.b)][static_cast<std::size_t>(FeatureId::History)] == 0.0);
    // The second a -> b (t=3100): now it had, once.
    const auto second = replayCase(data, data.transitions[2]);
    REQUIRE(second.b >= 0);
    CHECK(*second.features[static_cast<std::size_t>(second.b)][static_cast<std::size_t>(FeatureId::History)] > 0.0);
    CHECK(second.features.size() == 2);

    // The report.
    EvalReport r;
    r.date = "2026-11-26";
    r.tracks = 4;
    r.transitions = 3;
    r.sessions = 2;
    std::vector<ReplayCase> cases{first, replayCase(data, data.transitions[1]), second};
    for (const auto& s : weightSets(defaultPresets(), std::nullopt))
        r.results.push_back({s.name, evaluate(cases, s.weights)});
    r.tuned = tuneWeights(cases, defaultPresets().front().weights);
    r.tunedMetrics = evaluate(cases, *r.tuned);
    r.choices = choiceStats(*reader);
    const auto md = reportMarkdown(r);
    CHECK(md.find("# Track Matcher evaluation - 2026-11-26") != std::string::npos);
    CHECK(md.find("| Balanced |") != std::string::npos);
    CHECK(md.find("| Balanced - key |") != std::string::npos);
    CHECK(md.find("\"name\": \"Tuned\"") != std::string::npos);
    CHECK(md.find("Nothing logged yet") != std::string::npos);
    CHECK(md.find("Not trained yet") != std::string::npos);
}
