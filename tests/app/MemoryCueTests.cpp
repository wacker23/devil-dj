// P3-02: memory cues and loops. Done when: CRUD + call tested.

#include "MemoryCues.h"
#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "library/Connection.h"
#include "library/CueRepository.h"
#include "library/Database.h"
#include "library/TrackRepository.h"
#include "ui/Actions.h"
#include "ui/KeyboardController.h"
#include "ui/MemoryCuePanel.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>

using namespace devildj;
namespace fs = std::filesystem;

TEST_CASE("Memory cues: add, list, rename, recolour, delete, limits; they survive a restart", "[app][memorycue][library]")
{
    const auto dir = fs::temp_directory_path() /
                     ("devildj-memory-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    constexpr double kFile = 44100.0, kDeck = 48000.0;
    std::int64_t track = 0;
    {
        auto db = std::move(library::Database::open(dir / "library.db")).value();
        db->writeAndWait(
            [&](library::Connection& c)
            {
                library::Track t;
                t.path = "C:\\Music\\b.mp3";
                t.contentHash = "b";
                track = library::TrackRepository(c).add(t).value();
                // Out of order on purpose: the list comes back by position.
                CHECK(app::addMemoryCue(c, track, false, 30.0 * kDeck, 0.0, kFile, kDeck, "Drop"));
                CHECK(app::addMemoryCue(c, track, false, 10.0 * kDeck, 0.0, kFile, kDeck));
                CHECK(app::addMemoryCue(c, track, true, 20.0 * kDeck, 4.0 * kDeck, kFile, kDeck, "Loop"));
                // Not the same spot twice (within 10 ms), but a loop may start where a cue is.
                CHECK_FALSE(app::addMemoryCue(c, track, false, 10.005 * kDeck, 0.0, kFile, kDeck));
                CHECK(app::addMemoryCue(c, track, true, 10.0 * kDeck, 2.0 * kDeck, kFile, kDeck));
                CHECK_FALSE(app::addMemoryCue(c, track, true, 50.0 * kDeck, 0.0, kFile, kDeck)); // a loop needs a length
                CHECK_FALSE(app::addMemoryCue(c, 0, false, 5.0 * kDeck, 0.0, kFile, kDeck));       // not in the library
            });
    }

    auto db = std::move(library::Database::open(dir / "library.db")).value();
    auto reader = std::move(db->openReader()).value();
    auto list = app::loadMemoryCues(*reader, track, kFile, kDeck);
    REQUIRE(list.size() == 4);
    CHECK(std::abs(list[0].start - 10.0 * kDeck) < 1.1); // by position, at the deck's rate
    CHECK(list[2].loop);
    CHECK(list[2].label == "Loop");
    CHECK(std::abs(list[2].length - 4.0 * kDeck) < 1.1);
    CHECK(list[3].label == "Drop");

    // Rename, recolour, delete.
    db->writeAndWait(
        [&](library::Connection& c)
        {
            CHECK(app::renameMemoryCue(c, list[3].id, "Break"));
            CHECK(app::recolourMemoryCue(c, list[3].id, 0x3C7CE0u));
            CHECK(app::removeMemoryCue(c, list[0].id));
        });
    list = app::loadMemoryCues(*reader, track, kFile, kDeck);
    REQUIRE(list.size() == 3);
    CHECK(list[2].label == "Break");
    CHECK(list[2].colour == 0x3C7CE0u);
    // Hot cues of the track aren't in this list.
    db->writeAndWait(
        [&](library::Connection& c)
        {
            library::Cue hot;
            hot.trackId = track;
            hot.slot = 0;
            hot.startSample = 4410;
            CHECK(library::CueRepository(c).add(hot));
        });
    CHECK(app::loadMemoryCues(*reader, track, kFile, kDeck).size() == 3);

    // At most 10 memory cues (and, apart from them, 10 loops).
    db->writeAndWait(
        [&](library::Connection& c)
        {
            int added = 0;
            for (int i = 0; i < 12; ++i)
                added += app::addMemoryCue(c, track, false, (60.0 + i) * kDeck, 0.0, kFile, kDeck) ? 1 : 0;
            CHECK(added == 9); // there was one cue (at 30 s) already
            CHECK(app::addMemoryCue(c, track, true, 100.0 * kDeck, kDeck, kFile, kDeck)); // loops count apart
        });
    list = app::loadMemoryCues(*reader, track, kFile, kDeck);
    CHECK(std::count_if(list.begin(), list.end(), [](const auto& m) { return !m.loop; }) == app::kMaxMemoryCues);
    reader.reset();
    db.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Memory cues: call previous / next from the playhead; a call sets the deck's CUE point", "[app][memorycue]")
{
    std::vector<app::MemoryCue> cues{{1, false, 1000.0, 0.0, {}, "a"}, {2, true, 5000.0, 800.0, {}, "b"},
                                     {3, false, 9000.0, 0.0, {}, "c"}};
    CHECK(app::nextMemoryCue(cues, 0.0)->id == 1);
    CHECK(app::nextMemoryCue(cues, 1000.0)->id == 2); // the one at the playhead counts as passed
    CHECK(app::nextMemoryCue(cues, 4998.0)->id == 2);
    CHECK_FALSE(app::nextMemoryCue(cues, 9500.0).has_value());
    CHECK(app::previousMemoryCue(cues, 9500.0)->id == 3);
    CHECK(app::previousMemoryCue(cues, 9000.0)->id == 2);
    CHECK_FALSE(app::previousMemoryCue(cues, 1000.0).has_value());
    CHECK_FALSE(app::nextMemoryCue({}, 0.0).has_value());

    // What a call does in the engine: the CUE point moves there, and the playhead with a seek; CUE then returns there.
    engine::TrackBuffer track;
    track.sampleRate = 48000.0;
    track.audio.setSize(2, 48000 * 20);
    track.audio.clear();
    engine::AudioEngine e;
    e.prepare(48000.0, 256);
    float l[256], r[256];
    float* outs[] = {l, r};
    e.send(engine::LoadTrack{0, &track, 1});
    e.process(outs, 2, 256);
    e.send(engine::SetCuePoint{0, 5.0 * 48000.0});
    e.send(engine::Seek{0, 5.0 * 48000.0});
    e.process(outs, 2, 256);
    CHECK(e.readState().decks[0].cuePointSamples == 5.0 * 48000.0);
    CHECK(e.readState().decks[0].positionSamples == 5.0 * 48000.0);
    e.send(engine::Play{0});
    for (int i = 0; i < 20; ++i)
        e.process(outs, 2, 256);
    e.send(engine::CuePress{0}); // back cue: to the called cue, paused
    e.send(engine::CueRelease{0});
    e.process(outs, 2, 256);
    CHECK_FALSE(e.readState().decks[0].playing);
    CHECK(e.readState().decks[0].positionSamples == 5.0 * 48000.0);
    e.send(engine::SetCuePoint{0, 1e12}); // clamped to the track
    e.process(outs, 2, 256);
    CHECK(e.readState().decks[0].cuePointSamples == 48000.0 * 20);
}

TEST_CASE("Memory cues: the keys and the list panel", "[app][memorycue][ui][keys]")
{
    ui::ActionDispatcher d;
    ui::KeyboardController k(d);
    using DA = ui::DeckAction;
    const auto is = [&](std::uint16_t code, std::uint8_t m, ui::CommandId expected)
    {
        const auto got = k.lookup({code, m}, ui::KeyContext::Main);
        CHECK((got && *got == expected));
    };
    is(ui::scan::Z, ui::mods::Ctrl, ui::deckCommand(0, DA::MemoryAdd));
    is(ui::scan::M, ui::mods::Ctrl, ui::deckCommand(1, DA::MemoryAdd));
    is(ui::scan::Digit3, ui::mods::Ctrl, ui::deckCommand(0, DA::CallPrevious));
    is(ui::scan::Digit4, ui::mods::Ctrl, ui::deckCommand(0, DA::CallNext));
    is(ui::scan::Digit9, ui::mods::Ctrl, ui::deckCommand(1, DA::CallPrevious));
    is(ui::scan::Digit0, ui::mods::Ctrl, ui::deckCommand(1, DA::CallNext));
    CHECK(ui::findConflicts(ui::defaultKeymap()).empty());

    juce::ScopedJuceInitialiser_GUI gui;
    ui::MemoryCuePanel panel;
    int called = -1, added = 0, deleted = -1;
    panel.onCall = [&](int row) { called = row; };
    panel.onAdd = [&] { ++added; };
    panel.onDelete = [&](int row) { deleted = row; };
    panel.setRows({{"0:10.0", false, "", {}}, {"0:20.0", true, "Loop", 0x4CBF5Au}}, "Deck A");
    CHECK(panel.numRows() == 2);
    panel.addButton().onClick();
    CHECK(added == 1);
    panel.list().selectRow(1);
    panel.callRow(1);
    CHECK(called == 1);
    panel.callRow(5); // no such row
    CHECK(called == 1);
    juce::Image image(juce::Image::ARGB, 340, 320, true);
    juce::Graphics g(image);
    panel.paintEntireComponent(g, false);
}
