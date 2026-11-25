// P6-10: Track Matcher v2 features - the mix regions, vocal clash, mixability, energy / sound over the regions, and
// suggestions that change when A enters its outro.

#include "analysis/TrackFeatures.h"
#include "library/Connection.h"
#include "library/CueRepository.h"
#include "library/Database.h"
#include "library/PhraseRepository.h"
#include "library/TrackRepository.h"
#include "matcher/CandidateQuery.h"
#include "matcher/Features.h"
#include "matcher/MatcherService.h"
#include "matcher/Scorer.h"
#include "util/MixRegions.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>

using namespace devildj;
using namespace devildj::matcher;

namespace
{
TrackInfo track(std::int64_t id, double energy)
{
    TrackInfo t;
    t.id = id;
    t.contentHash = "h" + std::to_string(id);
    t.artist = "Artist " + std::to_string(id);
    t.bpm = 125.0;
    t.key = 21; // A minor
    t.energy = energy;
    return t;
}

std::vector<float> axis(int i)
{
    std::vector<float> v(32, 0.0f);
    v[static_cast<std::size_t>(i)] = 1.0f;
    return v;
}

const Scored& find(const std::vector<Scored>& list, std::int64_t id)
{
    for (const auto& s : list)
        if (s.track->id == id)
            return s;
    CHECK(false); // not in the list
    return list.front();
}
} // namespace

TEST_CASE("Mix regions: from the mix points, else the phrases, else the ends", "[matcher][v2]")
{
    const double span = 32.0 * 60.0 / 120.0; // 16 s at 120 BPM
    const std::vector<util::PhraseSpan> phrases{{0.5, 16}, {32.5, 32}, {96.5, 16}};
    auto r = util::mixRegions(phrases, std::nullopt, std::nullopt, 120.0, 130.0);
    CHECK(r.inStart == 0.5);
    CHECK(r.inEnd == 0.5 + span);
    CHECK(r.outEnd == 96.5); // before the outro phrase
    CHECK(r.outStart == 96.5 - span);
    CHECK(r.introBars == 16);
    CHECK(r.outroBars == 16);
    r = util::mixRegions(phrases, 8.5, 110.0, 120.0, 130.0);
    CHECK(r.inStart == 8.5);
    CHECK(r.outEnd == 110.0);
    r = util::mixRegions({}, std::nullopt, std::nullopt, 120.0, 130.0);
    CHECK(r.inStart == 0.0);
    CHECK(r.outEnd == 130.0);
    CHECK(r.outStart == 130.0 - span);
    CHECK(r.introBars == 0);
}

TEST_CASE("Vocal clash and mixability", "[matcher][v2]")
{
    CHECK_FALSE(vocalFit(std::nullopt, 0.5).value);
    CHECK(*vocalFit(0.0, 1.0).value == 1.0);
    CHECK(std::abs(*vocalFit(1.0, 1.0).value - 0.2) < 1e-12);
    CHECK(vocalFit(0.9, 0.8).reason == "Vocal clash");
    CHECK(vocalFit(0.1, 0.0).reason == "No vocals over the mix");

    // A long, sparse, instrumental intro is best; a short, loud, sung one worst; no phrases = 0.5.
    const RegionShape good{32, 0.2, 0.7, 0.0}, bad{4, 0.9, 0.6, 1.0};
    CHECK(regionQuality(good) == 1.0);
    CHECK(regionQuality(bad) < 0.3);
    CHECK(regionQuality({}) == 0.5);
    CHECK_FALSE(mixFit({}, {}).value);
    const auto f = mixFit({16, 0.3, 0.7, 0.1}, good);
    REQUIRE(f.value);
    CHECK(*f.value > 0.9);
    CHECK(f.reason == "Long intro");
    CHECK(*mixFit(bad, bad).value < *mixFit(good, good).value);
}

TEST_CASE("Matcher v2: suggestions change when A enters its outro", "[matcher][v2]")
{
    // A: a peak-time track (whole 0.9) whose outro drops to 0.3 and sounds different (axis 1, not axis 0).
    Context c;
    c.a = track(1, 0.9);
    c.a.energyOut = 0.3;
    c.a.sound = axis(0);
    c.a.soundOutro = axis(1);
    c.aBpmNow = 125.0;
    c.c10 = 0.0;
    c.c90 = 1.0;
    // B1 comes in at peak energy, sounding like A's body; B2 comes in low, sounding like A's outro.
    std::vector<TrackInfo> candidates{track(2, 0.9), track(3, 0.4)};
    candidates[0].energyIn = 0.9;
    candidates[0].soundIntro = axis(0);
    candidates[1].energyIn = 0.3;
    candidates[1].soundIntro = axis(1);
    const auto w = defaultPresets().front().weights; // a copy: the presets vector is a temporary

    c.aInOutro = false;
    auto top = rank(c, candidates, w, 2);
    REQUIRE(top.size() == 2);
    CHECK(top.front().track->id == 2); // in the body of A: keep the peak going
    const double bodyEnergyFit = *find(top, 3).features[static_cast<std::size_t>(FeatureId::Energy)].value;

    c.aInOutro = true;
    top = rank(c, candidates, w, 2);
    REQUIRE(top.size() == 2);
    CHECK(top.front().track->id == 3); // in A's outro: what follows the outro
    CHECK(*find(top, 3).features[static_cast<std::size_t>(FeatureId::Energy)].value > bodyEnergyFit);
}

TEST_CASE("Matcher v2: a vocal clash and a poor intro rank lower", "[matcher][v2]")
{
    Context c;
    c.a = track(1, 0.6);
    c.a.vocalOutro = 0.9;
    c.a.outroBars = 16;
    c.aBpmNow = 125.0;
    std::vector<TrackInfo> candidates{track(2, 0.6), track(3, 0.6)};
    candidates[0].vocalIntro = 0.9; // sings over A's sung outro
    candidates[0].introBars = 4;
    candidates[1].vocalIntro = 0.0; // a long instrumental intro
    candidates[1].introBars = 32;
    const auto top = rank(c, candidates, defaultPresets().front().weights, 2);
    REQUIRE(top.size() == 2);
    CHECK(top.front().track->id == 3);
    const auto& clash = find(top, 2);
    CHECK(*clash.features[static_cast<std::size_t>(FeatureId::Vocal)].value < 0.5);
    CHECK(clash.features[static_cast<std::size_t>(FeatureId::Vocal)].reason == "Vocal clash");
    CHECK(*find(top, 3).features[static_cast<std::size_t>(FeatureId::Mix)].value >
          *clash.features[static_cast<std::size_t>(FeatureId::Mix)].value);
}

TEST_CASE("Matcher v2: the triggers fire when A enters its outro", "[matcher][v2]")
{
    MatcherTriggers t;
    MatcherTriggers::Snapshot s;
    s.masterDeck = 0;
    s.trackIds = {1, 2};
    CHECK(t.update(s));
    s.masterFraction = 0.5;
    CHECK_FALSE(t.update(s));
    s.masterInOutro = true;
    CHECK(t.update(s));
    CHECK_FALSE(t.update(s));
}

TEST_CASE("Matcher v2: the library gives each track its regions' energy, sound, vocals and phrase lengths", "[matcher][v2]")
{
    const auto dir = std::filesystem::temp_directory_path() /
                     ("devildj-matcherv2-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        auto db = std::move(library::Database::open(dir / "library.db")).value();
        std::int64_t a = 0;
        db->writeAndWait(
            [&](library::Connection& conn)
            {
                library::Track t;
                t.path = "C:\\x\\a.wav";
                t.contentHash = "a";
                t.bpm = 120.0;
                t.musicalKey = 21;
                t.sampleRate = 48000;
                t.durationMs = 160000;
                a = library::TrackRepository(conn).add(t).value();
                // Energy windows of 32 s: 0.2, 0.4, 0.8, 0.8, 0.3.
                std::vector<analysis::EnergyWindow> windows;
                for (int i = 0; i < 5; ++i)
                    windows.push_back({32.0f * i, 32.0f, -10.0f, 1.0f, 0.2f, 1000.0f});
                const auto packed = analysis::packWindows(windows);
                const auto curve = analysis::packHalves(std::vector<float>{0.2f, 0.4f, 0.8f, 0.8f, 0.3f});
                auto* s = conn.prepare("INSERT INTO track_features (track_id, version, energy, energy_features, energy_curve, "
                                       "vocal_intro, vocal_outro) VALUES (?, 1, 0.5, ?, ?, 0.1, 0.7)");
                REQUIRE(s != nullptr);
                REQUIRE(s->bind(1, a).bind(2, std::span<const std::uint8_t>(packed)).bind(3, std::span<const std::uint8_t>(curve)).run());
                // Phrases: 16-bar intro at 0, the body, a 16-bar outro at 128 s.
                REQUIRE(library::PhraseRepository(conn).set(a, {{0.0, 16, 0.2}, {32.0, 48, 0.8}, {128.0, 16, 0.3}}, false));
                // A MIX OUT at 112 s.
                library::Cue cue;
                cue.trackId = a;
                cue.kind = library::CueKind::MixOut;
                cue.startSample = 112 * 48000;
                REQUIRE(library::CueRepository(conn).add(cue));
                // The outro's sound.
                const auto outro = analysis::packFloats(std::vector<float>(32, 1.0f));
                auto* e = conn.prepare("INSERT INTO track_embeddings (track_id, model, segment, dim, vec) VALUES (?, 'sound_v1', 'outro', 32, ?)");
                REQUIRE(e != nullptr);
                REQUIRE(e->bind(1, a).bind(2, std::span<const std::uint8_t>(outro)).run());
            });
        auto reader = std::move(db->openReader()).value();
        const auto t = loadTrack(*reader, a, 0);
        REQUIRE(t);
        CHECK(t->introBars == 16);
        CHECK(t->outroBars == 16);
        CHECK(t->vocalIntro == 0.1);
        CHECK(t->vocalOutro == 0.7);
        // Mix-in region 0..16 s: window 0 (0.2). Mix-out region 96..112 s: window 3 (0.8).
        REQUIRE(t->energyIn);
        CHECK(std::abs(*t->energyIn - 0.2) < 0.01);
        REQUIRE(t->energyOut);
        CHECK(std::abs(*t->energyOut - 0.8) < 0.01);
        CHECK(t->outStartSeconds == 96.0);
        CHECK(t->soundOutro.size() == 32);
        CHECK(t->soundIntro.empty());
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

