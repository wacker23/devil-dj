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
    played.clear();
    playingDeck = 0;
    upcoming = order.front();
}

void Automix::stop()
{
    on = false;
}

int Automix::itemAfter(int pos)
{
    const int n = static_cast<int>(order.size());
    if (n == 0)
        return -1;
    int next = pos + 1;
    if (next >= n)
    {
        if (!settings.repeat)
            return -1;
        next = 0; // round again
    }
    if (settings.smart && pickNext && n > 1)
    {
        // This round's remaining items (a new round: all but the one playing).
        const int current = pos >= 0 && pos < n ? order[static_cast<std::size_t>(pos)] : -1;
        std::vector<int> remaining;
        for (int i = next; i < n; ++i)
            if (order[static_cast<std::size_t>(i)] != current)
                remaining.push_back(order[static_cast<std::size_t>(i)]);
        if (!remaining.empty())
        {
            int choice = pickNext(current, remaining);
            if (std::find(remaining.begin(), remaining.end(), choice) == remaining.end())
                choice = remaining.front();
            const auto at = std::find(order.begin() + next, order.end(), choice);
            std::iter_swap(order.begin() + next, at);
        }
    }
    return order[static_cast<std::size_t>(next)];
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
        const auto points = mixPointsOf ? mixPointsOf(playingDeck) : MixPoints{};
        const double in = points.in ? std::max(0.0, *points.in) : firstDownbeat(grid); // P6-08: its MIX IN
        out.push_back(engine::SetCuePoint{playingDeck, in});
        out.push_back(engine::Seek{playingDeck, in});
        out.push_back(engine::Play{playingDeck});
        out.push_back(engine::SetParam{engine::ParamId::Crossfader, playingDeck == 0 ? 0.0f : 1.0f});
        position = 0;
        played.push_back(deckItem[playingDeck]);
        upcoming = itemAfter(position);
        return out;
    }

    // A transition we planned has finished: the other deck is playing and this one stopped - it becomes the
    // playing deck; the next track goes on the deck just freed.
    if (transitionPlanned && !state.transition.active && state.decks[other].playing)
    {
        transitionPlanned = false;
        playingDeck = other;
        position = position + 1 < static_cast<int>(order.size()) ? position + 1 : 0; // repeat: round again
        played.push_back(deckItem[playingDeck]);
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
    // P4-19: the pair's own transition (saved, else Auto), asked once when both tracks are on their decks.
    if (!planAsked)
    {
        planAsked = true;
        plan = transitionFor ? transitionFor(playingDeck, other) : std::nullopt;
        if (plan && plan->curves == nullptr)
            plan.reset();
        // The mix-out, decided now: the plan's (still 8 beats ahead, and it fits before the end), else the track's
        // MIX OUT (P6-08; Automix's own style from there, as long as fits), else 32 beats before the end.
        chosenOut = std::floor((endBeat - settings.mixOutBeats) / 4.0) * 4.0;
        chosenBeats = static_cast<float>(settings.transitionBeats);
        const auto points = mixPointsOf ? mixPointsOf(playingDeck) : MixPoints{};
        if (plan && plan->outBeat >= 0.0 && plan->outBeat > playing.beat + 8.0 &&
            plan->outBeat + plan->curves->beats <= endBeat + 1e-6)
            chosenOut = plan->outBeat;
        else if (!plan && points.out)
        {
            const double at = grid.beatAt(*points.out);
            if (at > playing.beat + 8.0 && endBeat - at >= 4.0)
            {
                chosenOut = at;
                chosenBeats = std::min(chosenBeats, static_cast<float>(std::floor((endBeat - at) / 4.0) * 4.0));
            }
        }
    }
    const double mixOutTrack = chosenOut;
    const float ownBeats = chosenBeats;
    const double toMaster = state.masterBeat - playing.beat; // the master clock = this deck's beats + an offset
    const double startMaster = std::round(mixOutTrack + toMaster);
    if (state.masterBeat < startMaster - 8.0)
        return out; // not yet: plan it 8 beats ahead
    // Cue the next one at its mix-in and hand the transition to the engine.
    const auto nextGrid = gridOf ? gridOf(other) : util::Beatgrid();
    const auto nextPoints = mixPointsOf ? mixPointsOf(other) : MixPoints{};
    const double in = plan && !nextGrid.empty() ? std::max(0.0, nextGrid.sampleAt(plan->inBeat))
                      : nextPoints.in             ? std::max(0.0, *nextPoints.in) // P6-08: its MIX IN
                                                  : firstDownbeat(nextGrid);
    out.push_back(engine::SetCuePoint{other, in});
    out.push_back(engine::Seek{other, in});
    out.push_back(engine::SetSync{other, true});
    engine::StartTransition start{playingDeck, other, ownBeats, settings.style,
                                  std::max(startMaster, std::ceil(state.masterBeat))};
    if (plan)
    {
        start.curves = plan->curves.get();
        curvesToSend = plan->curves;
    }
    out.push_back(start);
    plan.reset();
    planAsked = false;
    transitionPlanned = true;
    ++transitions;
    lastStart = std::max(startMaster, std::ceil(state.masterBeat));
    return out;
}

} // namespace devildj::app
