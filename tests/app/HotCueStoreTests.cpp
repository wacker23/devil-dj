// P3-01: hot cues in the library (persist across restarts) and on the keyboard pads.

#include "HotCueStore.h"
#include "library/Connection.h"
#include "library/CueRepository.h"
#include "library/Database.h"
#include "library/TrackRepository.h"
#include "ui/Actions.h"
#include "ui/KeyboardController.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>

using namespace devildj;
namespace fs = std::filesystem;

TEST_CASE("Hot cues persist across restarts: set, colour, label, delete, sort", "[app][hotcue][library]")
{
    const auto dir = fs::temp_directory_path() /
                     ("devildj-hotcues-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    constexpr double kFile = 44100.0, kDeck = 48000.0; // a 44.1 kHz file on a 48 kHz deck
    std::int64_t track = 0;
    std::array<double, engine::kHotCues> starts{}, lengths{};
    starts.fill(-1.0);
    {
        auto db = std::move(library::Database::open(dir / "library.db")).value();
        db->writeAndWait(
            [&](library::Connection& c)
            {
                library::Track t;
                t.path = "C:\\Music\\a.mp3";
                t.contentHash = "a";
                t.sampleRate = 44100;
                track = library::TrackRepository(c).add(t).value();
            });
        // A fresh track: nothing; then the engine has pads A and D set.
        auto reader = std::move(db->openReader()).value();
        auto known = app::loadHotCues(*reader, track, kFile, kDeck);
        for (const auto& h : known)
            CHECK_FALSE(h.set);
        starts[0] = 48000.0 * 12.5;
        starts[3] = 48000.0 * 61.25;
        auto changes = app::diffHotCues(known, starts, lengths, kFile, kDeck);
        REQUIRE(changes.size() == 2);
        CHECK(changes[0].start == std::llround(44100.0 * 12.5));
        CHECK(known[3].colour == app::defaultHotCueColour(3)); // a new cue gets its pad's colour
        CHECK(app::diffHotCues(known, starts, lengths, kFile, kDeck).empty()); // nothing new the second time
        // D renamed and coloured.
        known[3].label = "Drop";
        known[3].colour = 0x3C7CE0;
        changes.push_back(app::changeFor(known, 3, kFile, kDeck));
        db->writeAndWait([&](library::Connection& c) { CHECK(app::applyHotCueChanges(c, track, changes)); });
    }

    // Next start: the same cues, at the deck's rate.
    {
        auto db = std::move(library::Database::open(dir / "library.db")).value();
        auto reader = std::move(db->openReader()).value();
        auto known = app::loadHotCues(*reader, track, kFile, kDeck);
        REQUIRE(known[0].set);
        REQUIRE(known[3].set);
        CHECK(std::abs(known[0].start - 48000.0 * 12.5) < 1.1); // whole file samples in between
        CHECK(std::abs(known[3].start - 48000.0 * 61.25) < 1.1);
        CHECK(known[3].label == "Drop");
        CHECK(known[3].colour == 0x3C7CE0);
        CHECK(known[0].colour == app::defaultHotCueColour(0));
        const auto forEngine = app::toEngine(1, known);
        CHECK(forEngine.deck == 1);
        CHECK(forEngine.cues[3].set);
        CHECK(forEngine.cues[3].start == known[3].start);
        CHECK_FALSE(forEngine.cues[1].set);

        // The engine deletes A: removed from the library.
        std::array<double, engine::kHotCues> now{};
        now.fill(-1.0);
        now[3] = known[3].start;
        const auto changes = app::diffHotCues(known, now, lengths, kFile, kDeck);
        REQUIRE(changes.size() == 1);
        CHECK_FALSE(changes[0].set);
        db->writeAndWait([&](library::Connection& c) { CHECK(app::applyHotCueChanges(c, track, changes)); });
        CHECK_FALSE(library::CueRepository(*reader).hotCue(track, 0).has_value());
        CHECK(library::CueRepository(*reader).hotCue(track, 3).has_value());

        // Sort by time: D's cue (the only one) to pad A, its colour and label with it.
        auto sorted = app::sortedByTime(known);
        CHECK(sorted[0].set);
        CHECK(sorted[0].label == "Drop");
        CHECK_FALSE(sorted[3].set);
        std::vector<app::HotCueChange> all;
        for (int i = 0; i < engine::kHotCues; ++i)
            all.push_back(app::changeFor(sorted, i, kFile, kDeck));
        db->writeAndWait([&](library::Connection& c) { CHECK(app::applyHotCueChanges(c, track, all)); });
        const auto moved = library::CueRepository(*reader).hotCue(track, 0);
        REQUIRE(moved.has_value());
        CHECK(moved->label == "Drop");
        CHECK_FALSE(library::CueRepository(*reader).hotCue(track, 3).has_value());
        // Several cues sort by time.
        app::HotCues three;
        three[5] = {true, 300.0, 0.0, 1, "c"};
        three[1] = {true, 100.0, 0.0, 2, "a"};
        three[9] = {true, 200.0, 0.0, 3, "b"};
        const auto s3 = app::sortedByTime(three);
        CHECK(s3[0].label == "a");
        CHECK(s3[1].label == "b");
        CHECK(s3[2].label == "c");
        CHECK_FALSE(s3[3].set);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Hot cues on the keyboard pads: the keys, the two pages, Shift deletes", "[app][hotcue][keys]")
{
    // The pads' keys (KEYMAP.md): Q W E R A S D F (deck A), U I O P J K L ; (deck B).
    ui::ActionDispatcher d;
    ui::KeyboardController k(d);
    using DA = ui::DeckAction;
    const auto is = [&](std::uint16_t code, std::uint8_t m, ui::CommandId expected)
    {
        const auto got = k.lookup({code, m}, ui::KeyContext::Main);
        CHECK((got && *got == expected));
    };
    is(ui::scan::Q, ui::mods::None, ui::deckCommand(0, DA::Pad1));
    is(ui::scan::F, ui::mods::None, ui::deckCommand(0, DA::Pad8));
    is(ui::scan::U, ui::mods::None, ui::deckCommand(1, DA::Pad1));
    is(ui::scan::Semicolon, ui::mods::None, ui::deckCommand(1, DA::Pad8));
    is(ui::scan::Q, ui::mods::Shift, ui::deckCommand(0, DA::PadAlt1));
    // What a pad sends: page 1 = A-H, page 2 = I-P; release ends a hold; Shift + pad deletes.
    auto c = app::hotCueForPad(0, 0, 0, false, false);
    CHECK(c.slot == 0);
    CHECK(c.op == engine::HotCueOp::Press);
    c = app::hotCueForPad(1, 7, 1, false, true);
    CHECK(c.deck == 1);
    CHECK(c.slot == 15);
    CHECK(c.op == engine::HotCueOp::Release);
    c = app::hotCueForPad(0, 2, 1, true, false);
    CHECK(c.slot == 10);
    CHECK(c.op == engine::HotCueOp::Delete);
    CHECK(app::hotCueLetter(0) == 'A');
    CHECK(app::hotCueLetter(15) == 'P');
}
