// P6-07: smart cues - candidates from phrases, the rules, learning the owner's habit, the pads, the answers.

#include "app/SmartCues.h"
#include "library/Connection.h"
#include "library/CueFeedbackRepository.h"
#include "library/CueRepository.h"
#include "library/Database.h"
#include "library/PhraseRepository.h"
#include "library/TrackRepository.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <random>

using namespace devildj;
using namespace devildj::app;

namespace
{
// A dance track's phrases: intro, build, drop, break, drop, outro (start seconds, bars, energy).
std::vector<library::StoredPhrase> track(double offset = 0.0)
{
    return {{offset + 0.5, 16, 0.35}, {offset + 31.0, 8, 0.55}, {offset + 46.3, 32, 0.95},
            {offset + 107.3, 16, 0.4}, {offset + 137.8, 32, 0.95}, {offset + 198.8, 16, 0.45}};
}
constexpr double kDuration = 230.0;
} // namespace

TEST_CASE("Smart cues: candidates and their reasons", "[app][smartcues]")
{
    const auto c = cueCandidates(track(), kDuration);
    REQUIRE(c.size() == 6);
    CHECK(c[0].reason == CueReason::Start);
    CHECK(c[1].reason == CueReason::Rise);
    CHECK(c[2].reason == CueReason::Rise);
    CHECK(c[3].reason == CueReason::Break);
    CHECK(c[4].reason == CueReason::Drop); // the biggest rise (out of the break): the main drop
    CHECK(c[5].reason == CueReason::Outro);
    CHECK(c[4].features[6] == 1.0);
    CHECK(std::abs(c[5].features[0] - 198.8 / kDuration) < 1e-12);
    CHECK(c[5].features[5] == 1.0);
    CHECK(std::string(cueReasonName(CueReason::Drop)) == "DROP");
    CHECK(cueReasonFromName("OUTRO") == CueReason::Outro);
    CHECK(cueReasonFromName("nonsense") == CueReason::Phrase);
}

TEST_CASE("Smart cues: suggestions skip set cues and rejected ones, best first, in time order", "[app][smartcues]")
{
    CueHabits none;
    auto s = suggestCues(track(), kDuration, none, {}, {}, 1.0);
    REQUIRE(s.size() == 6); // all above the minimum by the rules here
    for (std::size_t i = 1; i < s.size(); ++i)
        CHECK(s[i].seconds > s[i - 1].seconds);
    s = suggestCues(track(), kDuration, none, {46.0}, {107.5}, 1.0, 3);
    REQUIRE(s.size() == 3);
    CHECK(s[0].reason == CueReason::Start); // the three best left: start, the build's rise, the main drop
    CHECK(s[1].seconds == 31.0);
    CHECK(s[2].reason == CueReason::Drop);
    // A flat track: only the start is worth a cue.
    const std::vector<library::StoredPhrase> flat{{0.5, 32, 0.8}, {61.0, 32, 0.82}, {121.5, 32, 0.79}};
    const auto f = suggestCues(flat, 180.0, none, {}, {}, 1.0);
    REQUIRE(f.size() == 1);
    CHECK(f[0].reason == CueReason::Start);
}

TEST_CASE("Smart cues: suggestions on the empty pads from A", "[app][smartcues]")
{
    const auto p = padsForSuggestions({true, false, false, true, false, false, false, false}, 3);
    CHECK(p == std::array<int, 8>{-1, 0, 1, -1, 2, -1, -1, -1});
    CHECK(padsForSuggestions({}, 0) == std::array<int, 8>{-1, -1, -1, -1, -1, -1, -1, -1});
}

TEST_CASE("Smart cues: the habit model learns which candidates the owner cues", "[app][smartcues]")
{
    // An owner who always cues the breaks (and never the drops), on 20 tracks.
    std::vector<CueHabits::Example> examples;
    for (int t = 0; t < 20; ++t)
    {
        const auto c = cueCandidates(track(t * 0.1), kDuration);
        std::vector<double> cued;
        for (const auto& x : c)
            if (x.reason == CueReason::Break)
                cued.push_back(x.seconds);
        for (const auto& e : cueExamples(c, cued, 0.5))
            examples.push_back(e);
    }
    CueHabits h;
    h.train(std::vector<CueHabits::Example>(examples.begin(), examples.begin() + 10));
    CHECK_FALSE(h.trained()); // too few yet
    h.train(examples);
    REQUIRE(h.trained());
    CHECK(h.positives() == 20);
    const auto c = cueCandidates(track(), kDuration);
    CHECK(h.probability(c[3].features) > 0.8); // the break
    CHECK(h.probability(c[4].features) < 0.3); // the drop
    // Blended: the break now ranks above the drop.
    const auto s = suggestCues(track(), kDuration, h, {}, {}, 1.0, 2);
    REQUIRE(s.size() == 2);
    CHECK(std::any_of(s.begin(), s.end(), [](const auto& x) { return x.reason == CueReason::Break; }));
    CHECK_FALSE(std::any_of(s.begin(), s.end(), [](const auto& x) { return x.reason == CueReason::Drop; }));
}

TEST_CASE("Smart cues: examples come from the library's cues and answers; the answers are kept", "[app][smartcues][library]")
{
    const auto dir = std::filesystem::temp_directory_path() /
                     ("devildj-smartcues-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        auto db = std::move(library::Database::open(dir / "library.db")).value();
        std::int64_t a = 0, b = 0, c = 0;
        db->writeAndWait(
            [&](library::Connection& conn)
            {
                library::TrackRepository tracks(conn);
                for (auto* id : {&a, &b, &c})
                {
                    library::Track t;
                    t.path = "C:\\x\\" + std::to_string(reinterpret_cast<std::uintptr_t>(id)) + ".wav";
                    t.contentHash = t.path;
                    t.sampleRate = 44100;
                    t.durationMs = static_cast<std::int64_t>(kDuration * 1000);
                    *id = tracks.add(t).value();
                    library::PhraseRepository(conn).set(*id, track(), false);
                }
                // a: a hot cue on the drop; b: an answer rejecting the break and accepting the outro; c: nothing.
                library::Cue cue;
                cue.trackId = a;
                cue.slot = 0;
                cue.startSample = static_cast<std::int64_t>(46.3 * 44100);
                REQUIRE(library::CueRepository(conn).add(cue));
                library::CueFeedbackRepository answers(conn);
                REQUIRE(answers.add({b, 107.3, "BREAK", false, 1}));
                REQUIRE(answers.add({b, 198.8, "OUTRO", true, 2}));
            });
        auto reader = std::move(db->openReader()).value();
        const auto examples = gatherCueExamples(*reader);
        CHECK(examples.size() == 12); // a and b: 6 candidates each; c has no labels
        int cued = 0;
        for (const auto& e : examples)
            cued += e.cued ? 1 : 0;
        CHECK(cued == 2); // a's first rise (its hot cue), b's outro
        library::CueFeedbackRepository answers(*reader);
        CHECK(answers.count(true) == 1);
        CHECK(answers.count(false) == 1);
        CHECK(answers.forTrack(b).size() == 2);
        CHECK(answers.forTrack(b)[0].reason == "BREAK");
        CHECK(answers.forTrack(c).empty());
        const CueAnswers counts{answers.count(true), answers.count(false)};
        CHECK(counts.rate() == 0.5);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
