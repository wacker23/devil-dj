// P2-14: the beatgrid model - mapping, edits, JSON, tap tempo - and its storage.

#include "library/BeatgridRepository.h"
#include "library/Connection.h"
#include "library/Database.h"
#include "library/TrackRepository.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>

using devildj::util::Beatgrid;
using devildj::util::GridAnchor;
using devildj::util::TapTempo;

namespace
{
bool near(double a, double b, double tolerance = 1e-6)
{
    return std::abs(a - b) <= tolerance;
}
} // namespace

TEST_CASE("Beatgrid: a constant grid maps samples and beats", "[util][grid]")
{
    const auto g = Beatgrid::constant(22050.0, 120.0, 44100.0); // first beat at 0.5 s, 0.5 s per beat
    CHECK_FALSE(g.empty());
    CHECK(near(g.sampleAt(0.0), 22050.0));
    CHECK(near(g.sampleAt(4.0), 22050.0 + 4 * 22050.0));
    CHECK(near(g.sampleAt(-1.0), 0.0)); // before the first anchor: same tempo
    CHECK(near(g.beatAt(22050.0 + 11025.0), 0.5));
    CHECK(near(g.bpmAt(1e9), 120.0));
    CHECK(g.nearestBeat(22050.0 * 3 + 100.0) == 2);
    CHECK(g.barBeatOf(0) == 1);
    CHECK(g.barBeatOf(5) == 2);
    CHECK(g.barBeatOf(-1) == 4);
    CHECK(Beatgrid().empty());
}

TEST_CASE("Beatgrid: tempo changes between anchors", "[util][grid]")
{
    Beatgrid g = Beatgrid::constant(0.0, 120.0, 48000.0);
    REQUIRE(g.addAnchor({8.0, 8 * 24000.0, 128.0, 1})); // 8 beats of 120, then 128
    CHECK_FALSE(g.addAnchor({4.0, 1.0, 100.0, 1}));     // out of order
    CHECK(near(g.sampleAt(8.0), 192000.0));
    CHECK(near(g.sampleAt(9.0), 192000.0 + 48000.0 * 60.0 / 128.0));
    CHECK(near(g.beatAt(192000.0 + 22500.0), 9.0));
    CHECK(near(g.bpmAt(10.0), 120.0));
    CHECK(near(g.bpmAt(200000.0), 128.0));
}

TEST_CASE("Beatgrid: the edits", "[util][grid]")
{
    const auto base = Beatgrid::constant(1000.0, 128.0, 44100.0);
    const double period = 44100.0 * 60.0 / 128.0;

    // 1 ms earlier / later.
    auto g = base;
    g.shiftSeconds(0.001);
    CHECK(near(g.sampleAt(0.0), 1000.0 + 44.1));
    g.shiftSeconds(-0.002);
    CHECK(near(g.sampleAt(0.0), 1000.0 - 44.1));

    // Set downbeat here: the nearest beat moves exactly to the position and becomes beat 1.
    g = base;
    const double here = 1000.0 + 6 * period + 300.0; // a little after beat 6
    g.setDownbeatAt(here);
    CHECK(near(g.sampleAt(6.0), here, 1e-6));
    CHECK(g.barBeatOf(6) == 1);
    CHECK(g.barBeatOf(7) == 2);
    CHECK(near(g.bpmAt(0.0), 128.0)); // the tempo doesn't change

    // Downbeat one beat later / earlier: the beats stay, the bar phase moves.
    g = base;
    g.shiftBeats(1);
    CHECK(g.barBeatOf(1) == 1);
    CHECK(near(g.sampleAt(0.0), 1000.0));
    g.shiftBeats(-2);
    CHECK(g.barBeatOf(3) == 1);

    // BPM x2 / /2: twice / half as many beats, the first beat stays.
    g = base;
    g.scaleTempo(2.0);
    CHECK(near(g.bpmAt(0.0), 256.0));
    CHECK(near(g.sampleAt(0.0), 1000.0));
    CHECK(near(g.sampleAt(2.0), base.sampleAt(1.0)));
    g.scaleTempo(0.5);
    CHECK(near(g.sampleAt(1.0), base.sampleAt(1.0)));

    // Another sample rate.
    const auto r = base.rescaled(48000.0);
    CHECK(near(r.sampleAt(3.0), base.sampleAt(3.0) * 48000.0 / 44100.0, 1e-6));
    CHECK(r.sampleRate() == 48000.0);
}

TEST_CASE("Beatgrid: JSON (the library's anchors_json)", "[util][grid]")
{
    Beatgrid g = Beatgrid::constant(22050.0, 127.5, 44100.0, 3);
    REQUIRE(g.addAnchor({64.0, 22050.0 + 64 * 44100.0 * 60.0 / 127.5, 128.0, 3}));
    const auto json = g.toJson();
    CHECK(json.rfind("[{\"beat\":0,\"sample\":22050,\"bpm\":127.5000,\"bar_beat\":3},{\"beat\":64,", 0) == 0);
    const auto back = Beatgrid::fromJson(json, 44100.0);
    REQUIRE(back.has_value());
    CHECK(back->size() == 2);
    CHECK(near(back->bpmAt(0.0), 127.5));
    CHECK(back->barBeatOf(0) == 3);
    CHECK(near(back->sampleAt(64.0), std::round(g.sampleAt(64.0)), 1e-6));
    // What the analysis and the importers write reads too.
    const auto analyzed = Beatgrid::fromJson("[{\"beat\":0,\"sample\":1103,\"bpm\":120.0000,\"bar_beat\":1}]", 44100.0);
    REQUIRE(analyzed.has_value());
    CHECK(near(analyzed->sampleAt(0.0), 1103.0));
    CHECK(Beatgrid::fromJson("[]", 44100.0)->empty());
    for (const char* bad : {"", "[", "[{}]", "[{\"sample\":1}]", "[{\"sample\":1,\"bpm\":\"x\"}]", "[1,2]", "{}"})
    {
        INFO(bad);
        CHECK_FALSE(Beatgrid::fromJson(bad, 44100.0).has_value());
    }
}

TEST_CASE("Tap tempo", "[util][grid]")
{
    TapTempo t;
    for (int i = 0; i < 3; ++i)
        t.tap(10.0 + i * 0.5);
    CHECK_FALSE(t.bpm().has_value()); // 4 taps needed
    t.tap(11.5);
    REQUIRE(t.bpm().has_value());
    CHECK(near(*t.bpm(), 120.0, 1e-9));
    // Uneven taps: the fit, not the last interval.
    for (const double at : {12.02, 12.48, 13.03})
        t.tap(at);
    CHECK(std::abs(*t.bpm() - 120.0) < 1.0);
    // A pause over 2 s starts over.
    t.tap(20.0);
    CHECK(t.count() == 1);
    CHECK_FALSE(t.bpm().has_value());
}

TEST_CASE("Beatgrid edits persist in the library", "[library][grid]")
{
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() /
                     ("devildj-grid-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::int64_t id = 0;
    Beatgrid edited;
    {
        auto db = std::move(devildj::library::Database::open(dir / "library.db")).value();
        db->writeAndWait(
            [&](devildj::library::Connection& c)
            {
                devildj::library::Track t;
                t.path = "C:\\Music\\a.mp3";
                t.contentHash = "a";
                t.sampleRate = 44100;
                t.bpm = 127.9;
                id = devildj::library::TrackRepository(c).add(t).value();
                CHECK(c.exec("INSERT INTO beatgrids (track_id, anchors_json) VALUES (" + std::to_string(id) +
                             ", '[{\"beat\":0,\"sample\":1103,\"bpm\":127.9000,\"bar_beat\":1}]')"));
            });
        // What a deck does: read at the file's rate, scale to its 48 kHz buffer, edit, save back at 44.1 kHz.
        auto reader = std::move(db->openReader()).value();
        const auto stored = devildj::library::BeatgridRepository(*reader).get(id);
        REQUIRE(stored.has_value());
        CHECK(stored->sampleRate == 44100);
        auto deckGrid = Beatgrid::fromJson(stored->anchorsJson, 44100.0)->rescaled(48000.0);
        deckGrid.scaleTempo(2.0);
        deckGrid.scaleTempo(0.5);
        deckGrid.shiftSeconds(0.010);
        deckGrid.setDownbeatAt(deckGrid.sampleAt(2.0));
        deckGrid.locked = true;
        edited = deckGrid.rescaled(44100.0);
        db->writeAndWait(
            [&](devildj::library::Connection& c)
            { CHECK(devildj::library::BeatgridRepository(c).set(id, edited.toJson(), true, edited.firstBpm())); });
    }
    // Next start: the same grid, locked, and the track's BPM / lock follow.
    auto db = std::move(devildj::library::Database::open(dir / "library.db")).value();
    auto reader = std::move(db->openReader()).value();
    const auto stored = devildj::library::BeatgridRepository(*reader).get(id);
    REQUIRE(stored.has_value());
    CHECK(stored->locked);
    const auto back = Beatgrid::fromJson(stored->anchorsJson, 44100.0);
    REQUIRE(back.has_value());
    CHECK(std::abs(back->sampleAt(0.0) - edited.sampleAt(0.0)) <= 0.5); // whole samples in the JSON
    CHECK(back->barBeatOf(2) == 1);
    CHECK(std::abs(back->bpmAt(0.0) - 127.9) < 1e-3);
    const auto track = *devildj::library::TrackRepository(*reader).get(id);
    CHECK(track.bpmLocked);
    CHECK(std::abs(*track.bpm - 127.9) < 1e-3);
    reader.reset();
    db.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
}
