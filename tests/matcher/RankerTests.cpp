// P6-12: the personal ranker - pairs from the log, training (recovers a synthetic owner's weight ordering; 10k pairs
// in under 1 s), non-negative weights, blending, the stored model, the scorer's interaction terms.

#include "library/Connection.h"
#include "library/Database.h"
#include "matcher/ChoiceLog.h"
#include "matcher/Ranker.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <numeric>
#include <random>

using namespace devildj;
using namespace devildj::matcher;

namespace
{
#ifdef NDEBUG
constexpr bool kDebugBuild = false;
#else
constexpr bool kDebugBuild = true;
#endif

// A synthetic owner: true weights over the 11 features (interactions 0). Each impression shows 10 candidates with
// random features; the owner picks one by a softmax of their own score (beta: how consistent they are).
std::vector<Impression> synthetic(const RankWeights& truth, int impressions, double beta, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::vector<Impression> out;
    for (int i = 0; i < impressions; ++i)
    {
        Impression m;
        m.id = i + 1;
        std::vector<double> scores;
        for (int c = 0; c < 10; ++c)
        {
            ShownItem s;
            s.track = 100 + c;
            s.rank = c + 1;
            double score = 0.0;
            for (int k = 0; k < kFeatureCount; ++k)
            {
                const double v = u(rng);
                s.x[static_cast<std::size_t>(k)] = v;
                score += truth[static_cast<std::size_t>(k)] * v;
            }
            for (int k = kFeatureCount; k < kRankDims; ++k)
                s.x[static_cast<std::size_t>(k)] = 0.0;
            scores.push_back(beta * score);
            m.shown.push_back(s);
        }
        const double top = *std::max_element(scores.begin(), scores.end());
        std::vector<double> p;
        for (const double s : scores)
            p.push_back(std::exp(s - top));
        std::discrete_distribution<int> pick(p.begin(), p.end());
        const int chosen = pick(rng);
        m.shown[static_cast<std::size_t>(chosen)].chosen = true;
        m.chosenRank = chosen + 1;
        out.push_back(m);
    }
    return out;
}

double spearman(const RankWeights& a, const RankWeights& b, int n)
{
    const auto ranks = [n](const RankWeights& v)
    {
        std::vector<int> idx(static_cast<std::size_t>(n));
        std::iota(idx.begin(), idx.end(), 0);
        std::sort(idx.begin(), idx.end(), [&](int x, int y) { return v[static_cast<std::size_t>(x)] < v[static_cast<std::size_t>(y)]; });
        std::vector<double> r(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            r[static_cast<std::size_t>(idx[static_cast<std::size_t>(i)])] = i;
        return r;
    };
    const auto ra = ranks(a), rb = ranks(b);
    double d2 = 0.0;
    for (int i = 0; i < n; ++i)
        d2 += (ra[static_cast<std::size_t>(i)] - rb[static_cast<std::size_t>(i)]) * (ra[static_cast<std::size_t>(i)] - rb[static_cast<std::size_t>(i)]);
    return 1.0 - 6.0 * d2 / (n * (static_cast<double>(n) * n - 1.0));
}

RankWeights truthWeights()
{
    // Unlike the Balanced preset (tempo, key and energy first): this owner goes by groove, vocals and tags.
    //            tempo key  energy sound groove vocal mix  tags hist rating fresh
    return {0.15, 0.4, 0.3, 0.6, 3.0, 2.2, 0.1, 1.4, 0.8, 0.05, 0.02, 0.0, 0.0, 0.0};
}
} // namespace

TEST_CASE("Ranker: pairs from the log - chosen over the rest, weighted (or turned round) by the outcome", "[matcher][ranker]")
{
    Impression m;
    for (int c = 0; c < 3; ++c)
    {
        ShownItem s;
        s.track = c;
        s.rank = c + 1;
        s.x[0] = 0.1 * c;
        s.x[1] = c == 2 ? std::nullopt : std::optional<double>(0.5);
        m.shown.push_back(s);
    }
    m.shown[1].chosen = true;
    auto pairs = rankPairs({m});
    REQUIRE(pairs.size() == 2);
    CHECK(pairs[0].weight == 1.0);
    CHECK(std::abs(pairs[0].d[0] - 0.1) < 1e-12); // chosen (0.1) - track 0 (0.0)
    CHECK(pairs[1].d[1] == 0.0);                  // track 2's value is missing: nothing said
    m.outcome = Outcome::Mixed;
    CHECK(rankPairs({m})[0].weight == 2.0);
    m.outcome = Outcome::ThumbsDown;
    pairs = rankPairs({m});
    CHECK(pairs[0].weight == 2.0);
    CHECK(std::abs(pairs[0].d[0] + 0.1) < 1e-12); // turned round
    m.outcome = Outcome::Ejected;
    CHECK(rankPairs({m})[0].weight == 0.5);
}

TEST_CASE("Ranker: a synthetic owner's weight ordering is recovered", "[matcher][ranker]")
{
    const auto truth = truthWeights();
    const auto log = synthetic(truth, kDebugBuild ? 300 : 1500, 4.0, 11);
    const auto pairs = rankPairs(log);
    const auto prior = rankWeightsOf(defaultPresets().front().weights); // Balanced: a different ordering
    const auto r = trainRanker(pairs, prior);
    const double rho = spearman(r.weights, truth, kFeatureCount), rhoPrior = spearman(prior, truth, kFeatureCount);
    INFO("Spearman learnt " << rho << " (the preset " << rhoPrior << "), loss " << r.loss << ", " << r.pairs << " pairs");
    CHECK(rho >= 0.85);
    CHECK(rho > rhoPrior + 0.2);
    // The top three in order: groove, vocal, tags.
    std::vector<int> idx(kFeatureCount);
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return r.weights[static_cast<std::size_t>(a)] > r.weights[static_cast<std::size_t>(b)]; });
    CHECK(idx[0] == static_cast<int>(FeatureId::Groove));
    CHECK(idx[1] == static_cast<int>(FeatureId::Vocal));
    CHECK(idx[2] == static_cast<int>(FeatureId::Tags));
    for (const double w : r.weights)
        CHECK(w >= 0.0);
    // Scaled to the preset's sum.
    double sum = 0.0, sumPrior = 0.0;
    for (int k = 0; k < kFeatureCount; ++k)
    {
        sum += r.weights[static_cast<std::size_t>(k)];
        sumPrior += prior[static_cast<std::size_t>(k)];
    }
    CHECK(std::abs(sum - sumPrior) < 1e-9);
}

TEST_CASE("Ranker: with little data it stays near the preset", "[matcher][ranker]")
{
    const auto prior = rankWeightsOf(defaultPresets().front().weights);
    const auto r = trainRanker(rankPairs(synthetic(truthWeights(), 3, 4.0, 5)), prior);
    double moved = 0.0;
    for (int k = 0; k < kFeatureCount; ++k)
        moved = std::max(moved, std::abs(r.weights[static_cast<std::size_t>(k)] - prior[static_cast<std::size_t>(k)]));
    INFO("largest change " << moved);
    CHECK(moved < 0.5);
    CHECK(trainRanker({}, prior).weights == prior);
}

TEST_CASE("Ranker: 10,000 pairs train in under a second (logged)", "[matcher][ranker][perf]")
{
    const auto pairs = rankPairs(synthetic(truthWeights(), 1112, 4.0, 3)); // 9 pairs each: 10,008
    REQUIRE(pairs.size() >= 10000);
    const auto r = trainRanker(pairs, rankWeightsOf(defaultPresets().front().weights));
    WARN("Ranker: " << r.pairs << " pairs trained in " << r.seconds * 1000.0 << " ms");
    if (!kDebugBuild)
        CHECK(r.seconds < 1.0);
}

TEST_CASE("Ranker: blending by the number of impressions", "[matcher][ranker]")
{
    CHECK(blendAlpha(0) == 0.0);
    CHECK(blendAlpha(200) == 0.5);
    CHECK(std::abs(blendAlpha(1800) - 0.9) < 1e-12);
    const auto preset = defaultPresets().front().weights;
    RankWeights learnt{};
    learnt[static_cast<std::size_t>(FeatureId::Key)] = 4.0;
    learnt[kFeatureCount] = 1.0;
    const auto none = blendWeights(preset, learnt, 0);
    CHECK(none.w == preset.w);
    CHECK(none.interactions == std::array<double, 3>{});
    const auto half = blendWeights(preset, learnt, 200);
    CHECK(half[FeatureId::Key] == 0.5 * preset[FeatureId::Key] + 0.5 * 4.0);
    CHECK(half[FeatureId::Tempo] == 0.5 * preset[FeatureId::Tempo]);
    CHECK(half.interactions[0] == 0.5);
}

TEST_CASE("Ranker: the scorer's interaction terms (0 in the presets)", "[matcher][ranker]")
{
    Context c;
    c.a.id = 1;
    c.a.bpm = 125.0;
    c.a.key = 21;
    c.a.energy = 0.4;
    c.aBpmNow = 125.0;
    c.intent = Intent::Up;
    TrackInfo b = c.a;
    b.id = 2;
    b.energy = 0.6;
    auto w = defaultPresets().front().weights;
    const double plain = score(c, b, w).score;
    w.interactions[0] = 2.0; // a liking for energy going up when building
    const double boosted = score(c, b, w).score;
    CHECK(boosted > plain);
    c.intent = Intent::Hold;
    CHECK(score(c, b, w).score == score(c, b, defaultPresets().front().weights).score);
}

TEST_CASE("Ranker: the model in the library - trained from the log, stored, reset", "[matcher][ranker][library]")
{
    const auto dir = std::filesystem::temp_directory_path() /
                     ("devildj-ranker-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        auto db = std::move(library::Database::open(dir / "library.db")).value();
        db->writeAndWait(
            [&](library::Connection& c)
            {
                CHECK_FALSE(loadPersonalModel(c));
                const auto preset = defaultPresets().front().weights;
                CHECK_FALSE(retrainPersonalModel(c, preset, "Balanced", 100)); // nothing logged
                ChoiceLogRepository log(c);
                for (const auto& m : synthetic(truthWeights(), 60, 4.0, 9))
                {
                    auto copy = m;
                    copy.id = 0;
                    REQUIRE(log.add(copy));
                }
                const auto first = retrainPersonalModel(c, preset, "Balanced", 100);
                REQUIRE(first);
                CHECK(first->version == 1);
                CHECK(first->impressions == 60);
                const auto again = retrainPersonalModel(c, preset, "Balanced", 200);
                REQUIRE(again);
                CHECK(again->version == 2);
                const auto stored = loadPersonalModel(c);
                REQUIRE(stored);
                CHECK(stored->trainedAt == 200);
                CHECK(stored->basedOn == "Balanced");
                for (int k = 0; k < kRankDims; ++k)
                    CHECK(std::abs(stored->weights[static_cast<std::size_t>(k)] - again->weights[static_cast<std::size_t>(k)]) < 1e-5);
                CHECK(resetPersonalModel(c));
                CHECK_FALSE(loadPersonalModel(c));
            });
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
