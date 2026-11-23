// P4-19: saved transitions in Automix. Done when: a 10-track Automix plays its custom transitions unattended and on
// the beat.

#include "Automix.h"
#include "app/TransitionPresets.h"
#include "app/TransitionStore.h"
#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "library/Connection.h"
#include "library/Database.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <vector>

using namespace devildj;
namespace fs = std::filesystem;

TEST_CASE("Saved transitions: one per track pair, saved, read back, replaced, deleted; the playlist labels",
          "[app][transition][saved][library]")
{
    const auto dir = fs::temp_directory_path() /
                     ("devildj-saved-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        auto db = std::move(library::Database::open(dir / "library.db")).value();
        app::SavedTransition t;
        t.fromTrack = 7;
        t.toTrack = 9;
        t.preset = "Bass swap";
        t.curves = app::makePreset(app::TransitionPreset::BassSwap, 24.0f);
        t.curves.lane(true, engine::Lane::EqMid).points = {{0.0f, -6.0f}, {12.0f, 0.0f}};
        t.outBeat = 160.0;
        t.inBeat = 2.0;
        bool ok = false;
        db->writeAndWait([&](library::Connection& c) { ok = app::saveTransition(c, t); });
        CHECK(ok);
        auto reader = std::move(db->openReader()).value();
        const auto back = app::loadTransition(*reader, 7, 9);
        REQUIRE(back);
        CHECK(back->preset == "Bass swap");
        CHECK(back->outBeat == 160.0);
        CHECK(back->inBeat == 2.0);
        CHECK(back->curves.beats == 24.0f);
        for (int k = 0; k < 2; ++k)
            for (int l = 0; l < engine::kLaneCount; ++l)
                for (double b = 0.0; b <= 24.0; b += 1.5)
                    CHECK(std::abs(engine::evaluate((k == 0 ? back->curves.from : back->curves.to)[static_cast<std::size_t>(l)], b) -
                                   engine::evaluate((k == 0 ? t.curves.from : t.curves.to)[static_cast<std::size_t>(l)], b)) < 1e-3f);
        CHECK_FALSE(app::loadTransition(*reader, 9, 7)); // the other way round is another pair
        // Saved again: replaced.
        t.preset = "Custom";
        t.outBeat = 128.0;
        db->writeAndWait([&](library::Connection& c) { app::saveTransition(c, t); });
        CHECK(app::loadTransition(*reader, 7, 9)->outBeat == 128.0);
        app::SavedTransition other = t;
        other.fromTrack = 9;
        other.toTrack = 11;
        other.preset = "Echo out";
        db->writeAndWait([&](library::Connection& c) { app::saveTransition(c, other); });
        // The labels for the playlists' Mix column.
        app::TransitionLabels labels;
        labels.load(*reader);
        CHECK(labels.size() == 2);
        CHECK(labels.get(7, 9) == "Custom");
        CHECK(labels.get(9, 11) == "Echo out");
        CHECK(labels.get(11, 9).isEmpty());
        labels.set(1, 2, "");
        CHECK(labels.get(1, 2) == "Custom");
        labels.erase(1, 2);
        CHECK(labels.get(1, 2).isEmpty());
        // Deleted.
        db->writeAndWait([&](library::Connection& c) { app::deleteTransition(c, 7, 9); });
        CHECK_FALSE(app::loadTransition(*reader, 7, 9));
        // Tracks without a library id can't have one.
        app::SavedTransition none;
        db->writeAndWait([&](library::Connection& c) { ok = app::saveTransition(c, none); });
        CHECK_FALSE(ok);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

struct Track
{
    std::unique_ptr<engine::TrackBuffer> buffer;
    util::Beatgrid grid;
};

Track makeTrack(double bpm, int beats, double offset, double hz)
{
    Track t;
    t.buffer = std::make_unique<engine::TrackBuffer>();
    t.buffer->sampleRate = kRate;
    const double beatSamples = 60.0 / bpm * kRate;
    const int n = static_cast<int>(offset * kRate + beats * beatSamples + kRate);
    t.buffer->audio.setSize(2, n);
    for (int i = 0; i < n; ++i)
    {
        const double sinceBeat = std::fmod(std::max(0.0, i - offset * kRate), beatSamples);
        const double click = sinceBeat < 400.0 ? 0.6 * std::exp(-sinceBeat / 80.0) : 0.0;
        const auto v = static_cast<float>(0.1 * std::sin(2.0 * 3.141592653589793 * hz * i / kRate) + click);
        t.buffer->audio.setSample(0, i, v);
        t.buffer->audio.setSample(1, i, v);
    }
    t.grid = util::Beatgrid::constant(offset * kRate, bpm, kRate);
    return t;
}
} // namespace

TEST_CASE("Saved transitions in Automix: 10 tracks unattended, each pair's own transition, each on its planned beat",
          "[app][automix][saved]")
{
    std::vector<Track> tracks;
    for (int i = 0; i < 10; ++i)
        tracks.push_back(makeTrack(118.0 + i, 96, 0.05 + 0.031 * i, 200.0 + 40.0 * i));
    engine::AudioEngine e;
    e.prepare(kRate, kBlock);
    float l[kBlock], r[kBlock];
    float* outs[] = {l, r};
    std::array<int, 2> onDeck{-1, -1};
    app::Automix automix;
    automix.load = [&](engine::DeckId deck, int index)
    {
        e.send(engine::LoadTrack{deck, tracks[static_cast<std::size_t>(index)].buffer.get(), static_cast<engine::BufferId>(index + 1)});
        e.send(engine::SetBeatgrid{deck, tracks[static_cast<std::size_t>(index)].grid});
        onDeck[deck] = index;
        automix.loaded(deck, index);
    };
    automix.gridOf = [&](engine::DeckId deck)
    { return onDeck[deck] >= 0 ? tracks[static_cast<std::size_t>(onDeck[deck])].grid : util::Beatgrid(); };
    // Each pair's own transition: a different preset and length, starting at bar 17 of the outgoing track (beat 64)
    // and bringing the incoming one in from its bar 2 (beat 4).
    const app::TransitionPreset presets[] = {app::TransitionPreset::QuickCut, app::TransitionPreset::BassSwap,
                                             app::TransitionPreset::SmoothBlend, app::TransitionPreset::FilterRise};
    const float lengths[] = {4.0f, 16.0f, 32.0f, 8.0f};
    int asked = 0;
    std::vector<float> plannedBeats;
    automix.transitionFor = [&](engine::DeckId from, engine::DeckId to) -> std::optional<app::Automix::PlannedTransition>
    {
        const int k = onDeck[from] % 4;
        ++asked;
        CHECK(onDeck[to] == onDeck[from] + 1); // asked for the right pair
        plannedBeats.push_back(lengths[k]);
        return app::Automix::PlannedTransition{std::make_shared<const engine::TransitionCurves>(app::makePreset(presets[k], lengths[k])), 64.0, 4.0};
    };
    automix.setQueue(10);
    automix.start();
    int transitions = 0;
    bool wasActive = false;
    std::vector<double> deckBeatAtStart, startBeats, lengthsPlayed;
    std::vector<int> fromTracks;
    for (int block = 0; block < static_cast<int>(900.0 * kRate / kBlock); ++block)
    {
        if (block % 16 == 0)
            for (const auto& c : automix.tick(e.readState()))
            {
                const auto* st = std::get_if<engine::StartTransition>(&c);
                if (st != nullptr && st->curves != nullptr)
                    e.startTransition(*st, automix.takeCurves());
                else
                    e.send(c);
            }
        e.process(outs, 2, kBlock);
        const auto& s = e.readState();
        const bool running = s.transition.active && !s.transition.waiting;
        if (running && !wasActive)
        {
            ++transitions;
            CHECK(s.transition.curves);
            startBeats.push_back(s.transition.startBeat);
            lengthsPlayed.push_back(s.transition.beats);
            deckBeatAtStart.push_back(s.decks[s.transition.from].beat);
            fromTracks.push_back(onDeck[s.transition.from]);
        }
        wasActive = running;
        if (transitions == 9 && !s.transition.active && !s.decks[0].playing != !s.decks[1].playing && automix.nextIndex() < 0)
            break;
    }
    CHECK(transitions == 9);
    CHECK(asked == 9);
    REQUIRE(startBeats.size() == 9);
    for (std::size_t i = 0; i < 9; ++i)
    {
        INFO("transition " << i);
        CHECK(startBeats[i] == std::floor(startBeats[i]));             // on a beat of the master clock
        CHECK(std::abs(deckBeatAtStart[i] - 64.0) < 0.05);              // where it was planned on the outgoing track
        CHECK(lengthsPlayed[i] == plannedBeats[i]);                     // its own length
        CHECK(fromTracks[i] == static_cast<int>(i));                    // in queue order
    }
    // The incoming deck came in from beat 4 of its grid: deck 1's first transition brought track 1 in there.
    CHECK(e.readState().beatLateEvents == 0);
}
