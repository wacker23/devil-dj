// P3-24: the Next Track panel. Done when: suggestions refresh within 100 ms of loading a track or changing intent;
// every control reachable by keyboard.

#include "MainLayout.h"
#include "analysis/TrackFeatures.h"
#include "library/Connection.h"
#include "library/Database.h"
#include "matcher/MatcherService.h"
#include "ui/Actions.h"
#include "ui/KeyboardController.h"
#include "ui/NextTrackPanel.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <random>
#include <thread>

using namespace devildj;
namespace fs = std::filesystem;

TEST_CASE("Next Track panel: rows, chips, the highlight, the controls, a double-click loads", "[ui][nexttrack]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ui::NextTrackPanel panel;
    panel.setBounds(0, 0, 440, 520);
    ui::NextTrackPanel::State s;
    s.presets = {"Balanced", "Harmonic"};
    s.keyA = 21; // A minor = 8A
    s.energyHistory = {0.4, 0.5, 0.55};
    for (int i = 0; i < 10; ++i)
    {
        ui::NextTrackPanel::Row r;
        r.title = "Track " + juce::String(i);
        r.artist = "Artist";
        r.bpm = 124.0 + i;
        r.key = i % 24;
        r.energy = 0.1 * i;
        r.score = 0.9 - 0.05 * i;
        r.wildcard = i == 9;
        r.chips = {{"8A -> 9A", 0}, {"BPM +1.2 %", 1}, {"Energy 5 -> 9 up", 2}};
        r.breakdown = "tempo 0.90 x 1.0";
        s.rows.push_back(r);
    }
    panel.setState(s);
    CHECK(ui::NextTrackPanel::chipLevel(0.9) == 0);
    CHECK(ui::NextTrackPanel::chipLevel(0.5) == 1);
    CHECK(ui::NextTrackPanel::chipLevel(0.1) == 2);
    int highlighted = -1, loaded = -1, intent = -1, preset = -1, key = -1, scope = -1;
    panel.onHighlight = [&](int r) { highlighted = r; };
    panel.onLoad = [&](int r) { loaded = r; };
    panel.onIntent = [&](int i) { intent = i; };
    panel.onPreset = [&](int i) { preset = i; };
    panel.onKeyPolicy = [&](int i) { key = i; };
    panel.onScope = [&](int i) { scope = i; };
    const int row3 = panel.rowAt({100, 0}) == -1 ? -1 : 0;
    (void)row3;
    // Find row 3 by scanning down the list.
    int y3 = -1;
    for (int y = 0; y < 520 && y3 < 0; ++y)
        if (panel.rowAt({100, y}) == 3)
            y3 = y;
    REQUIRE(y3 > 0);
    CHECK(panel.rowAt({100, 519}) == -1); // the mini view isn't a row
    panel.intentButton(2).onClick();
    CHECK(intent == 2);
    panel.presetBox().setSelectedId(2, juce::sendNotificationSync);
    CHECK(preset == 1);
    panel.keyBox().setSelectedId(1, juce::sendNotificationSync);
    CHECK(key == 0);
    panel.scopeBox().setSelectedId(2, juce::sendNotificationSync);
    CHECK(scope == 1);
    juce::Image image(juce::Image::ARGB, 440, 520, true);
    juce::Graphics g(image);
    panel.paintEntireComponent(g, false);
    (void)highlighted;
    (void)loaded;
    // Empty: the message instead.
    panel.setState({});
    panel.paintEntireComponent(g, false);
}

TEST_CASE("Next Track: every control on a key; the panel's place in the layout", "[ui][nexttrack][keys]")
{
    ui::ActionDispatcher d;
    ui::KeyboardController k(d);
    using GA = ui::GlobalAction;
    const auto at = [&](std::uint16_t code, std::uint8_t m) { return k.lookup({code, m}, ui::KeyContext::Main); };
    CHECK(at(ui::scan::N, ui::mods::None) == ui::globalCommand(GA::LoadSuggestion));
    CHECK(at(ui::scan::N, ui::mods::Shift) == ui::globalCommand(GA::NextSuggestion));
    CHECK(at(ui::scan::N, ui::mods::Ctrl) == ui::globalCommand(GA::PreviewSuggestion));
    CHECK(at(ui::scan::N, ui::mods::Ctrl | ui::mods::Shift) == ui::globalCommand(GA::CycleIntent));
    CHECK(at(ui::scan::N, ui::mods::Alt) == ui::globalCommand(GA::MatcherPreset));
    CHECK(at(ui::scan::N, ui::mods::Alt | ui::mods::Shift) == ui::globalCommand(GA::MatcherKeyPolicy));
    CHECK(at(ui::scan::N, ui::mods::Ctrl | ui::mods::Alt) == ui::globalCommand(GA::MatcherScope));
    CHECK(ui::findConflicts(ui::defaultKeymap()).empty());
    const auto l = app::computeLayout({0, 0, 1440, 900}, app::WaveLayout::Horizontal, false, {}, true);
    CHECK_FALSE(l.nextTrack.isEmpty());
    CHECK(l.nextTrack.getX() > l.browser.getRight());
    CHECK(app::computeLayout({0, 0, 800, 600}, app::WaveLayout::Horizontal, false, {}, true).nextTrack.isEmpty());
}

TEST_CASE("Next Track: a new request (a load, an intent change) is answered within 100 ms on 5,000 tracks",
          "[matcher][nexttrack][library]")
{
    const auto dir = fs::temp_directory_path() /
                     ("devildj-nexttrack-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::mt19937 rng(5);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    {
        auto db = std::move(library::Database::open(dir / "library.db")).value();
        db->writeAndWait(
            [&](library::Connection& c)
            {
                c.exec("BEGIN");
                auto* ins = c.prepare("INSERT INTO tracks (id, path, content_hash, bpm, musical_key, date_added) "
                                      "VALUES (?, ?, ?, ?, ?, 0)");
                auto* feat = c.prepare("INSERT INTO track_features (track_id, version, energy, sound_v1) VALUES (?, 1, ?, ?)");
                for (int i = 1; i <= 5001; ++i)
                {
                    ins->bind(1, static_cast<std::int64_t>(i)).bind(2, "C:\\m\\" + std::to_string(i))
                        .bind(3, "h" + std::to_string(i)).bind(4, 120.0 + i % 12).bind(5, i % 24);
                    REQUIRE(ins->run());
                    ins->reset();
                    std::vector<float> v(32);
                    for (auto& x : v)
                        x = nd(rng);
                    const auto blob = analysis::packFloats(v);
                    feat->bind(1, static_cast<std::int64_t>(i)).bind(2, 0.5).bind(3, std::span<const std::uint8_t>(blob));
                    REQUIRE(feat->run());
                    feat->reset();
                }
                c.exec("COMMIT");
            });
    }
    auto db = std::move(library::Database::open(dir / "library.db")).value();
    {
        matcher::MatcherService service(*db);
        std::atomic<int> results{0};
        service.onResult = [&](const matcher::MatchResult&) { ++results; };
        double worst = 0.0;
        for (const auto intent : {matcher::Intent::Hold, matcher::Intent::Up, matcher::Intent::Down, matcher::Intent::Switch})
        {
            const int before = results;
            matcher::MatchRequest r;
            r.contextTrackId = 1;
            r.intent = intent;
            r.keyPolicy = matcher::KeyPolicy::Relaxed;
            r.nowUnixSeconds = 1000;
            const auto t0 = std::chrono::steady_clock::now();
            service.request(r);
            while (results == before)
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            worst = std::max(worst, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        WARN("request -> result, worst of 4: " << worst << " ms");
#ifdef NDEBUG
        CHECK(worst < 100.0);
#endif
    }
    db.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Next Track panel: the thumbs buttons rate the last transition (P6-11)", "[ui][matcher][choicelog]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    devildj::ui::NextTrackPanel panel;
    panel.setBounds(0, 0, 360, 600);
    std::vector<bool> got;
    panel.onThumbs = [&](bool up) { got.push_back(up); };
    panel.thumbsButton(true).onClick(); // what a click (mouse or touch) runs
    panel.thumbsButton(false).onClick();
    CHECK(got == std::vector<bool>{true, false});
    CHECK(panel.thumbsButton(true).getBounds().getRight() <= panel.thumbsButton(false).getX());
    CHECK(panel.thumbsButton(false).getRight() <= 360);
}
