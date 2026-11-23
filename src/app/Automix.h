#pragma once

#include "engine/EngineCommand.h"
#include "engine/EngineState.h"
#include "util/Beatgrid.h"

#include <array>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

namespace devildj::app
{

// Automix (P3-18): plays a queue of tracks unattended on decks A and B, each one mixed into the next with an
// auto-transition (P3-17) that starts on a bar 32 beats before the playing track's end (its mix-out) and brings the
// next one in from its first downbeat (its mix-in), synced. Repeat (round again) and random (a shuffled order) are
// options. Pure logic on the message thread: tick() looks at the engine state and asks for what is needed - a load
// (the app loads it and calls loaded() when it is on the deck), engine commands (sent by the caller).
class Automix
{
public:
    struct Settings
    {
        int transitionBeats = 16; // 8 / 16 / 32
        engine::TransitionStyle style = engine::TransitionStyle::Blend;
        bool repeat = false;
        bool random = false;
        // P3-25: at each transition the best-scoring remaining queue item (pickNext) instead of the queue's order.
        bool smart = false;
        double mixOutBeats = 32.0; // before the end
    };

    // The app: put queue item `index` on `deck` (asynchronously is fine) and call loaded(deck, index) when there.
    std::function<void(engine::DeckId deck, int index)> load;
    // The deck's beatgrid, in its buffer's samples (empty if none).
    std::function<util::Beatgrid(engine::DeckId deck)> gridOf;
    // Smart mode: the queue item to play after `current` out of `remaining` (the app scores them with the matcher);
    // anything not in `remaining` means the first of them.
    std::function<int(int current, const std::vector<int>& remaining)> pickNext;

    void setQueue(int count);   // items 0 .. count - 1 (the app keeps what they are)
    void setSettings(const Settings& s) { settings = s; }
    const Settings& getSettings() const noexcept { return settings; }
    void start();
    void stop();
    bool running() const noexcept { return on; }
    void loaded(engine::DeckId deck, int index);

    // Once in a while (the app's timer; tests every few blocks): returns the engine commands to send now.
    std::vector<engine::EngineCommand> tick(const engine::EngineState& state);

    // What it is doing (for the panel and the tests).
    int playingIndex() const noexcept { return deckItem[static_cast<std::size_t>(playingDeck)]; }
    int nextIndex() const noexcept { return upcoming; }
    int transitionsPlanned() const noexcept { return transitions; }
    double lastTransitionStart() const noexcept { return lastStart; }
    // Every queue item in the order it started playing (smart mode's choices; logged by the app).
    const std::vector<int>& playedOrder() const noexcept { return played; }

private:
    int itemAfter(int position); // smart mode chooses it (and moves it to the next place in `order`)
    void requestLoad(engine::DeckId deck);

    Settings settings;
    bool on = false;
    std::vector<int> order; // queue indices in play order
    int position = -1;      // where in `order` the playing track is
    int upcoming = -1;      // the queue index to load next
    std::array<int, 2> deckItem{-1, -1};    // which queue item each deck has (-1 none)
    std::array<bool, 2> deckReady{};        // loaded and cued
    std::array<bool, 2> loadAsked{};
    engine::DeckId playingDeck = 0;
    bool transitionPlanned = false;
    int transitions = 0;
    double lastStart = -1.0;
    std::vector<int> played;
    std::mt19937 rng{20261122u};
};

// The first downbeat of a grid at or after the track's start (its mix-in): the sample.
double firstDownbeat(const util::Beatgrid& grid);

} // namespace devildj::app
