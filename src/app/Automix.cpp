#include "Automix.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace devildj::app
{

double firstDownbeat(const util::Beatgrid& grid)
{
    if (grid.empty())
        return 0.0;
    auto k = static_cast<long long>(std::ceil(grid.beatAt(0.0) - 1e-9));
    for (int guard = 0; guard < 8 && grid.barBeatOf(k) != 1; ++guard)
        ++k;
    return std::max(0.0, grid.sampleAt(static_cast<double>(k)));
}

void Automix::setQueue(int count)
{
    order.resize(static_cast<std::size_t>(std::max(0, count)));
    std::iota(order.begin(), order.end(), 0);
    if (settings.random)
        std::shuffle(order.begin(), order.end(), rng);
    position = -1;
}

void Automix::start()
{
    if (order.empty())
        return;
    on = true;
    position = -1;
    deckItem = {-1, -1};
    deckReady = {false, false};
    loadAsked = {false, false};
    transitionPlanned = false;
    transitions = 0;
    playingDeck = 0;
    upcoming = order.front();
}

void Automix::stop()
{
    on = false;
}

int Automix::itemAfter(int pos) const
{
    if (order.empty())
        return -1;
    if (pos + 1 < static_cast<int>(order.size()))
        return order[static_cast<std::size_t>(pos + 1)];
    return settings.repeat ? order.front() : -1;
}

void Automix::requestLoad(engine::DeckId deck)
{
    if (upcoming < 0 || loadAsked[deck])
        return;
    loadAsked[deck] = true;
    deckReady[deck] = false;
    if (load)
        load(deck, upcoming);
}

void Automix::loaded(engine::DeckId deck, int index)
{
    if (deck > 1)
        return;
    deckItem[deck] = index;
    deckReady[deck] = true;
    loadAsked[deck] = false;
}

std::vector<engine::EngineCommand> Automix::tick(const engine::EngineState& state)
{
    std::vector<engine::EngineCommand> out;
    if (!on)
        return out;
    const auto other = static_cast<engine::DeckId>(1 - playingDeck);
    const auto& playing = state.decks[playingDeck];

    // Nothing playing yet: the first track on deck A, from its first downbeat.
    if (position < 0)
    {
        if (!deckReady[playingDeck])
        {
            requestLoad(playingDeck);
            return out;
        }
        const auto grid = gridOf ? gridOf(playingDeck) : util::Beatgrid();
        const double in = firstDownbeat(grid);
        out.push_back(engine::SetCuePoint{playingDeck, in});
        out.push_back(engine::Seek{playingDeck, in});
        out.push_back(engine::Play{playingDeck});
        out.push_back(engine::SetParam{engine::ParamId::Crossfader, playingDeck == 0 ? 0.0f : 1.0f});
        position = 0;
        upcoming = itemAfter(position);
        return out;
    }

    // A transition we planned has finished: the other deck is playing and this one stopped - it becomes the
    // playing deck; the next track goes on the deck just freed.
    if (transitionPlanned && !state.transition.active && state.decks[other].playing)
    {
        transitionPlanned = false;
        playingDeck = other;
        ++position;
        deckReady[1 - playingDeck] = false;
        deckItem[1 - playingDeck] = -1;
        upcoming = itemAfter(position);
        if (upcoming < 0 && !settings.repeat)
            return out; // the last one plays out
        return out;
    }
    if (transitionPlanned)
        return out; // under way

    // The next track on the other deck, cued at its mix-in, synced, its lows prepared by the transition.
    if (upcoming >= 0 && !deckReady[other])
    {
        requestLoad(other);
        return out;
    }
    if (upcoming < 0 || !playing.playing || !playing.hasGrid)
        return out;

    // The mix-out: the bar 32 beats before the playing track's end; in master clock beats.
    const auto grid = gridOf ? gridOf(playingDeck) : util::Beatgrid();
    if (grid.empty() || playing.lengthSamples <= 0)
        return out;
    const double endBeat = grid.beatAt(static_cast<double>(playing.lengthSamples));
    const double mixOutTrack = std::floor((endBeat - settings.mixOutBeats) / 4.0) * 4.0;
    const double toMaster = state.masterBeat - playing.beat; // the master clock = this deck's beats + an offset
    const double startMaster = std::round(mixOutTrack + toMaster);
    if (state.masterBeat < startMaster - 8.0)
        return out; // not yet: plan it 8 beats ahead
    // Cue the next one at its mix-in and hand the transition to the engine.
    const auto nextGrid = gridOf ? gridOf(other) : util::Beatgrid();
    const double in = firstDownbeat(nextGrid);
    out.push_back(engine::SetCuePoint{other, in});
    out.push_back(engine::Seek{other, in});
    out.push_back(engine::SetSync{other, true});
    out.push_back(engine::StartTransition{playingDeck, other, static_cast<float>(settings.transitionBeats),
                                          settings.style, std::max(startMaster, std::ceil(state.masterBeat))});
    transitionPlanned = true;
    ++transitions;
    lastStart = std::max(startMaster, std::ceil(state.masterBeat));
    return out;
}

} // namespace devildj::app
