// P6-06: phrase detection - boundaries on the bars, phrase lengths, energy, and the F-measure on generated tracks.

#include "PhraseEval.h"
#include "analysis/AnalysisService.h"
#include "analysis/PhraseDetector.h"
#include "engine/TrackDecoder.h"
#include "library/BeatgridRepository.h"
#include "library/Connection.h"
#include "library/Database.h"
#include "library/PhraseRepository.h"
#include "library/TrackRepository.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include <chrono>
#include <filesystem>
#include <map>

using namespace devildj;
using namespace devildj::analysis;

namespace
{
#ifdef NDEBUG
constexpr bool kDebugBuild = false;
#else
constexpr bool kDebugBuild = true;
#endif

std::vector<double> boundariesOf(const PhraseAnalysis& a)
{
    std::vector<double> out;
    for (std::size_t i = 1; i < a.phrases.size(); ++i)
        out.push_back(a.phrases[i].startSeconds);
    return out;
}
} // namespace

TEST_CASE("Phrases: boundary scoring", "[analysis][phrases]")
{
    const auto s = scoreBoundaries({10.0, 20.1, 35.0}, {10.0, 20.0, 30.0, 40.0}, 0.2);
    CHECK(s.hits == 2);
    CHECK(s.precision() == 2.0 / 3.0);
    CHECK(s.recall() == 0.5);
    CHECK(std::abs(s.f() - 2.0 * (2.0 / 3.0) * 0.5 / (2.0 / 3.0 + 0.5)) < 1e-12);
    CHECK(scoreBoundaries({10.0, 10.05}, {10.0}, 0.2).hits == 1); // a reference counts once
    CHECK(scoreBoundaries({}, {}, 0.2).f() == 0.0);
}

TEST_CASE("Phrases: no grid or no audio, no phrases", "[analysis][phrases]")
{
    std::vector<float> silence(22050 * 10, 0.0f);
    CHECK(detectPhrases(silence.data(), static_cast<int>(silence.size()), 22050.0, util::Beatgrid()).phrases.empty());
    CHECK(detectPhrases(nullptr, 0, 22050.0, util::Beatgrid::constant(0.0, 120.0, 22050.0)).phrases.empty());
}

TEST_CASE("Phrases: a track with clear sections - boundaries on its bars, lengths from the grid, energy", "[analysis][phrases]")
{
    // Seed 3's plan, checked to have no extensions or subtle boundaries is not assumed: any generated track works
    // as long as the detector finds its section starts on bars.
    const auto t = test::makePhraseTrack(7);
    const auto a = detectPhrases(t.mono.data(), static_cast<int>(t.mono.size()), t.rate, t.grid);
    REQUIRE(a.phrases.size() >= 4);
    INFO("sections " << t.description);
    // Every phrase starts on a bar of the grid and is 4..32 bars (the last may be shorter).
    const double bar = 4.0 * 60.0 / t.grid.firstBpm();
    for (std::size_t i = 0; i < a.phrases.size(); ++i)
    {
        const auto& p = a.phrases[i];
        const double bars = (p.startSeconds - a.phrases[0].startSeconds) / bar;
        CHECK(std::abs(bars - std::round(bars)) < 1e-6);
        if (i + 1 < a.phrases.size())
        {
            CHECK(p.bars >= 4);
            CHECK(p.bars <= 32);
            CHECK(p.bars % 4 == 0);
        }
        CHECK(p.energy >= 0.0);
        CHECK(p.energy <= 1.0);
    }
    CHECK(std::abs(a.phrases[0].startSeconds - t.grid.sampleAt(0.0) / t.rate) < 1e-6); // from the first downbeat
    const auto score = scoreBoundaries(boundariesOf(a), t.boundaries, 0.25 * bar);
    INFO("F " << score.f() << " (" << score.hits << " of " << score.reference << ", " << score.detected << " found)");
    CHECK(score.recall() >= 0.8);
    // The drops are the loudest phrases, the breaks and intro quieter.
    double maxEnergy = 0.0;
    for (const auto& p : a.phrases)
        maxEnergy = std::max(maxEnergy, p.energy);
    CHECK(maxEnergy > 0.9);
    CHECK(a.phrases.front().energy < maxEnergy - 0.1); // the intro
}

TEST_CASE("Phrases: the evaluation set - boundary F-measure (logged)", "[analysis][phrases][eval]")
{
    const int tracks = kDebugBuild ? 4 : 40;
    BoundaryScore exact, loose;
    std::map<int, BoundaryScore> byLength;
    double seconds = 0.0, audio = 0.0;
    for (int i = 0; i < tracks; ++i)
    {
        const auto t = test::makePhraseTrack(1000u + static_cast<unsigned>(i));
        const auto t0 = std::chrono::steady_clock::now();
        const auto a = detectPhrases(t.mono.data(), static_cast<int>(t.mono.size()), t.rate, t.grid);
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        audio += static_cast<double>(t.mono.size()) / t.rate;
        const double bar = 4.0 * 60.0 / t.grid.firstBpm();
        exact += scoreBoundaries(boundariesOf(a), t.boundaries, 0.25 * bar); // on the bar
        loose += scoreBoundaries(boundariesOf(a), t.boundaries, 1.1 * bar);  // within a bar
        // Recall by the length of the section before the boundary (extensions: 12, 20, 36 ...).
        for (std::size_t s = 0; s + 1 < t.sectionBars.size(); ++s)
            byLength[t.sectionBars[s]] += scoreBoundaries(boundariesOf(a), {t.boundaries[s]}, 0.25 * bar);
    }
    juce::String lengths;
    for (const auto& [bars, s] : byLength)
        lengths << bars << " bars: " << s.hits << "/" << s.reference << "  ";
    WARN("Phrase boundaries on " << tracks << " generated tracks: on the bar F " << exact.f() << " (P " << exact.precision()
                                 << ", R " << exact.recall() << "; " << exact.hits << " of " << exact.reference << ", "
                                 << exact.detected << " found); within a bar F " << loose.f() << ". Recall after a section of "
                                 << lengths << "Speed " << audio / std::max(1e-9, seconds) << " x real time.");
    CHECK(exact.f() >= 0.75);
}

// Tuning on other seeds than the evaluation (hidden: run by hand).
TEST_CASE("Phrases: parameter sweep on the tuning seeds", "[.phrasetune]")
{
    std::vector<test::PhraseTrack> set;
    for (unsigned i = 0; i < 30; ++i)
        set.push_back(test::makePhraseTrack(5000u + i));
    for (const double threshold : {0.15, 0.22, 0.3, 0.38, 0.46})
        for (const double bonus : {0.0, 0.04, 0.1})
            for (const double shortPenalty : {0.15, 0.25, 0.4})
            {
                PhraseParams p;
                p.threshold = threshold;
                p.gridBonus = bonus;
                p.shortPenalty = shortPenalty;
                BoundaryScore s;
                for (const auto& t : set)
                {
                    const auto a = detectPhrases(t.mono.data(), static_cast<int>(t.mono.size()), t.rate, t.grid, p);
                    s += scoreBoundaries(boundariesOf(a), t.boundaries, 0.25 * 4.0 * 60.0 / t.grid.firstBpm());
                }
                WARN("threshold " << threshold << " bonus " << bonus << " short " << shortPenalty << ": F " << s.f()
                                  << " P " << s.precision() << " R " << s.recall());
            }
}

// The owner's own ground truth (P6-06): tracks whose phrases were edited in the app. Hidden; reads the library at
// DEVILDJ_LIBRARY (default %APPDATA%\Devil-DJ\library.db) and the tracks' files. Nothing is changed.
TEST_CASE("Phrases: F-measure on the owner's edited tracks (logged)", "[.phraseeval]")
{
    auto path = juce::SystemStats::getEnvironmentVariable("DEVILDJ_LIBRARY", {});
    if (path.isEmpty())
        path = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Devil-DJ/library.db").getFullPathName();
    if (!juce::File(path).existsAsFile())
        SKIP("No library at " << path.toStdString());
    auto db = library::Database::open(std::filesystem::path(path.toWideCharPointer()));
    REQUIRE(db);
    auto reader = std::move(db.value()->openReader()).value();
    library::PhraseRepository phrases(*reader);
    const auto ids = phrases.editedTracks();
    if (ids.empty())
        SKIP("No track has edited phrases yet (right-click the waveform to correct them)");
    BoundaryScore exact;
    int used = 0;
    for (const auto id : ids)
    {
        const auto track = library::TrackRepository(*reader).get(id);
        const auto stored = library::BeatgridRepository(*reader).get(id);
        if (!track || !stored || !stored->sampleRate)
            continue;
        auto decoded = engine::decodeTrack(juce::File(juce::String::fromUTF8(track->path.c_str())));
        if (!decoded)
            continue;
        const auto& b = *decoded.value();
        DecodedAudio audio;
        audio.left = b.audio.getReadPointer(0);
        audio.right = b.audio.getReadPointer(b.audio.getNumChannels() > 1 ? 1 : 0);
        audio.numSamples = b.numSamples();
        audio.sampleRate = b.sampleRate;
        const auto mono = AnalysisService::monoSignal(audio);
        const auto grid = util::Beatgrid::fromJson(stored->anchorsJson, *stored->sampleRate);
        if (!grid)
            continue;
        const auto a = detectPhrases(mono.data(), static_cast<int>(mono.size()), AnalysisInput::kMonoRate,
                                     grid->rescaled(AnalysisInput::kMonoRate));
        std::vector<double> reference;
        const auto truth = phrases.get(id).phrases;
        for (std::size_t i = 1; i < truth.size(); ++i)
            reference.push_back(truth[i].startSeconds);
        const double bar = 4.0 * 60.0 / std::max(1.0, grid->firstBpm());
        const auto s = scoreBoundaries(boundariesOf(a), reference, 0.25 * bar);
        WARN(track->title << ": F " << s.f() << " (" << s.hits << " of " << s.reference << ", " << s.detected << " found)");
        exact += s;
        ++used;
    }
    WARN("Phrase boundaries on " << used << " of the owner's edited tracks: F " << exact.f() << " (P "
                                 << exact.precision() << ", R " << exact.recall() << ")");
}
