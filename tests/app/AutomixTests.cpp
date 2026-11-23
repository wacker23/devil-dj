// P3-18: Automix. Done when: 10-track automix plays unattended, all transitions on beat.

#include "Automix.h"
#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace devildj;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

struct Track
{
    std::unique_ptr<engine::TrackBuffer> buffer;
    util::Beatgrid grid;
};

// A track of `beats` beats at `bpm`: a click on every beat (louder on the downbeats) over a quiet tone; its grid
// starts `offset` seconds in.
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

TEST_CASE("Automix: 10 tracks unattended, every transition planned on a beat and the incoming deck on it",
          "[app][automix]")
{
    std::vector<Track> tracks;
    for (int i = 0; i < 10; ++i) // 118 ... 127 BPM, 80 beats each, grids starting at different points
        tracks.push_back(makeTrack(118.0 + i, 80, 0.05 + 0.031 * i, 200.0 + 40.0 * i));

    engine::AudioEngine e;
    e.prepare(kRate, kBlock);
    float l[kBlock], r[kBlock];
    float* outs[] = {l, r};
    std::array<int, 2> onDeck{-1, -1};
    std::vector<int> loadedOrder;
    app::Automix automix;
    automix.load = [&](engine::DeckId deck, int index)
    {
        e.send(engine::LoadTrack{deck, tracks[static_cast<std::size_t>(index)].buffer.get(),
                                 static_cast<engine::BufferId>(index + 1)});
        e.send(engine::SetBeatgrid{deck, tracks[static_cast<std::size_t>(index)].grid});
        onDeck[deck] = index;
        loadedOrder.push_back(index);
        automix.loaded(deck, index);
    };
    automix.gridOf = [&](engine::DeckId deck)
    { return onDeck[deck] >= 0 ? tracks[static_cast<std::size_t>(onDeck[deck])].grid : util::Beatgrid(); };
    automix.setQueue(10);
    automix.start();

    int transitions = 0;
    bool wasActive = false;
    std::vector<double> starts;
    double worstPhaseMs = 0.0;
    int checkAt = -1;
    for (int block = 0; block < static_cast<int>(600.0 * kRate / kBlock); ++block)
    {
        if (block % 16 == 0)
            for (const auto& c : automix.tick(e.readState()))
                e.send(c);
        e.process(outs, 2, kBlock);
        const auto& s = e.readState();
        if (s.transition.active && !wasActive)
        {
            ++transitions;
            starts.push_back(s.transition.startBeat);
        }
        // Once each transition is 2 beats in: the incoming deck in phase with the master clock.
        if (s.transition.active && !s.transition.waiting && s.masterBeat >= s.transition.startBeat + 2.0 && checkAt != transitions)
        {
            checkAt = transitions;
            worstPhaseMs = std::max(worstPhaseMs, std::abs(s.decks[s.transition.to].phaseErrorMs));
            CHECK(s.decks[s.transition.to].playing);
            CHECK(s.decks[s.transition.to].synced);
        }
        wasActive = s.transition.active;
        if (transitions == 9 && !s.transition.active && !s.decks[0].playing != !s.decks[1].playing &&
            automix.nextIndex() < 0)
            break; // the last one is playing out
    }
    WARN("9 transitions, worst phase error 2 beats in: " << worstPhaseMs << " ms");
    CHECK(transitions == 9);
    REQUIRE(starts.size() == 9);
    for (const double b : starts)
        CHECK(b == std::floor(b)); // on a beat of the master clock
    CHECK(worstPhaseMs < 1.0);
    const std::vector<int> inOrder{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(loadedOrder == inOrder);
}

TEST_CASE("Automix: the mix-in is the first downbeat; random plays each once; repeat goes round", "[app][automix]")
{
    auto grid = util::Beatgrid::constant(0.3 * kRate, 120.0, kRate);
    CHECK(std::abs(app::firstDownbeat(grid) - 0.3 * kRate) < 1e-6);
    CHECK(app::firstDownbeat(util::Beatgrid()) == 0.0);

    app::Automix automix;
    std::vector<int> asked;
    automix.load = [&](engine::DeckId, int index) { asked.push_back(index); };
    app::Automix::Settings settings;
    settings.random = true;
    automix.setSettings(settings);
    automix.setQueue(6);
    automix.start();
    engine::EngineState state;
    automix.tick(state);
    REQUIRE(asked.size() == 1);
    CHECK(asked[0] >= 0);
    CHECK(asked[0] < 6);
}

TEST_CASE("Automix smart: 10 tracks unattended, each next one the best remaining pick, the order recorded",
          "[app][automix][smart]")
{
    // Tempos out of order; the "matcher" here prefers the nearest tempo - smart order then follows the tempos.
    const double bpms[] = {120.0, 126.0, 121.0, 125.0, 122.0, 127.0, 123.0, 124.0, 119.0, 128.0};
    std::vector<Track> tracks;
    for (int i = 0; i < 10; ++i)
        tracks.push_back(makeTrack(bpms[i], 72, 0.04 + 0.02 * i, 220.0 + 30.0 * i));
    engine::AudioEngine e;
    e.prepare(kRate, kBlock);
    float l[kBlock], r[kBlock];
    float* outs[] = {l, r};
    std::array<int, 2> onDeck{-1, -1};
    app::Automix automix;
    automix.load = [&](engine::DeckId deck, int index)
    {
        e.send(engine::LoadTrack{deck, tracks[static_cast<std::size_t>(index)].buffer.get(),
                                 static_cast<engine::BufferId>(index + 1)});
        e.send(engine::SetBeatgrid{deck, tracks[static_cast<std::size_t>(index)].grid});
        onDeck[deck] = index;
        automix.loaded(deck, index);
    };
    automix.gridOf = [&](engine::DeckId deck)
    { return onDeck[deck] >= 0 ? tracks[static_cast<std::size_t>(onDeck[deck])].grid : util::Beatgrid(); };
    int picks = 0;
    automix.pickNext = [&](int current, const std::vector<int>& remaining)
    {
        ++picks;
        int best = remaining.front();
        for (const int i : remaining)
            if (std::abs(bpms[i] - bpms[current]) < std::abs(bpms[best] - bpms[current]) ||
                (std::abs(bpms[i] - bpms[current]) == std::abs(bpms[best] - bpms[current]) && bpms[i] > bpms[best]))
                best = i;
        return best;
    };
    app::Automix::Settings settings;
    settings.smart = true;
    automix.setSettings(settings);
    automix.setQueue(10);
    automix.start();
    int transitions = 0;
    bool wasActive = false;
    for (int block = 0; block < static_cast<int>(600.0 * kRate / kBlock); ++block)
    {
        if (block % 16 == 0)
            for (const auto& c : automix.tick(e.readState()))
                e.send(c);
        e.process(outs, 2, kBlock);
        const auto& s = e.readState();
        if (s.transition.active && !wasActive)
            ++transitions;
        wasActive = s.transition.active;
        if (transitions == 9 && !s.transition.active && !s.decks[0].playing != !s.decks[1].playing &&
            automix.nextIndex() < 0)
            break;
    }
    CHECK(transitions == 9);
    CHECK(picks == 9);
    // From 120: 121, 122, 123, 124, 125, 126, 127, 128 (higher on a tie), then 119 last.
    const std::vector<int> expected{0, 2, 4, 6, 7, 3, 1, 5, 9, 8};
    CHECK(automix.playedOrder() == expected);
}

TEST_CASE("Automix: repeat goes round the whole queue again (plain and smart)", "[app][automix]")
{
    for (const bool smart : {false, true})
    {
        app::Automix automix;
        std::array<int, 2> deckOf{-1, -1};
        automix.load = [&](engine::DeckId deck, int index)
        {
            deckOf[deck] = index;
            automix.loaded(deck, index);
        };
        automix.gridOf = [](engine::DeckId) { return util::Beatgrid::constant(0.0, 120.0, kRate); };
        automix.pickNext = [](int, const std::vector<int>& remaining) { return remaining.back(); };
        app::Automix::Settings settings;
        settings.repeat = true;
        settings.smart = smart;
        automix.setSettings(settings);
        automix.setQueue(3);
        automix.start();
        engine::EngineState state;
        state.decks[0].hasGrid = state.decks[1].hasGrid = true;
        state.decks[0].lengthSamples = state.decks[1].lengthSamples = static_cast<std::int64_t>(60.0 * kRate);
        // Drive it by hand: each tick either plans a transition (jump the clock near the end) or finishes it.
        int playing = 0;
        for (int step = 0; step < 200 && automix.playedOrder().size() < 7; ++step)
        {
            state.decks[static_cast<std::size_t>(playing)].playing = true;
            state.masterBeat = 1000.0 + step * 200.0;
            state.decks[static_cast<std::size_t>(playing)].beat = 116.0;
            const auto before = automix.transitionsPlanned();
            automix.tick(state);
            if (automix.transitionsPlanned() != before)
            {
                state.transition.active = false;
                state.decks[static_cast<std::size_t>(playing)].playing = false;
                playing = 1 - playing;
                state.decks[static_cast<std::size_t>(playing)].playing = true;
            }
        }
        const auto& order = automix.playedOrder();
        REQUIRE(order.size() >= 7);
        // Each round of three has every item once.
        for (int round = 0; round < 2; ++round)
        {
            std::vector<int> items(order.begin() + round * 3, order.begin() + round * 3 + 3);
            std::sort(items.begin(), items.end());
            CHECK(items == std::vector<int>{0, 1, 2});
        }
        if (!smart)
            CHECK(order == std::vector<int>{0, 1, 2, 0, 1, 2, 0});
    }
}
