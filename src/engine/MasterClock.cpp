#include "engine/MasterClock.h"

#include <algorithm>
#include <cmath>

namespace devildj::engine
{

void MasterClock::prepare(double sampleRate) noexcept
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
}

void MasterClock::reset() noexcept
{
    currentMode = Mode::Auto;
    chosenDeck = kInternal;
    internal = 120.0;
    start = 0;
    length = 0;
    tempo = internal;
    beatStart = 0.0;
    source = kInternal;
    barOffset = 0.0;
}

void MasterClock::setMode(Mode mode, int deck) noexcept
{
    currentMode = mode;
    chosenDeck = deck;
}

void MasterClock::setInternalBpm(double bpm) noexcept
{
    if (std::isfinite(bpm))
        internal = std::clamp(bpm, kMinBpm, kMaxBpm);
}

int MasterClock::chooseSource(std::span<const ClockDeckInput> decks) const noexcept
{
    const auto usable = [&](int i)
    {
        const auto& d = decks[static_cast<std::size_t>(i)];
        return d.playing && d.hasGrid && std::isfinite(d.beat) && std::isfinite(d.bpm) && d.bpm >= kMinBpm &&
               d.bpm <= kMaxBpm;
    };
    switch (currentMode)
    {
        case Mode::Internal:
            return kInternal;
        case Mode::Deck:
            return chosenDeck >= 0 && chosenDeck < static_cast<int>(decks.size()) && usable(chosenDeck) ? chosenDeck
                                                                                                        : kInternal;
        case Mode::Auto:
            // Stay with the leading deck while it is usable; else the first playing synced deck; else the first
            // playing deck (a synced deck needs someone to follow).
            if (source >= 0 && source < static_cast<int>(decks.size()) && usable(source))
                return source;
            for (int i = 0; i < static_cast<int>(decks.size()); ++i)
                if (usable(i) && decks[static_cast<std::size_t>(i)].synced)
                    return i;
            for (int i = 0; i < static_cast<int>(decks.size()); ++i)
                if (usable(i))
                    return i;
            return kInternal;
    }
    return kInternal;
}

void MasterClock::beginBlock(int numSamples, std::span<const ClockDeckInput> decks) noexcept
{
    // Where the last block ended: the new block starts there.
    const double continued = beatAtBlockEnd();
    start += length;
    length = std::max(0, numSamples);

    const int next = chooseSource(decks);
    if (next != kInternal)
    {
        const auto& d = decks[static_cast<std::size_t>(next)];
        if (next != source) // a deck takes over: keep the bar count, take its phase
            barOffset = kBeatsPerBar * std::round((continued - d.beat) / kBeatsPerBar);
        beatStart = d.beat + barOffset;
        tempo = d.bpm;
        internal = std::clamp(d.bpm, kMinBpm, kMaxBpm); // if it stops, the clock runs on at its tempo
    }
    else
    {
        beatStart = continued;
        tempo = internal;
    }
    source = next;
}

double MasterClock::beatAt(double sampleTime) const noexcept
{
    return beatStart + (sampleTime - static_cast<double>(start)) * beatsPerSample();
}

double MasterClock::sampleAt(double beat) const noexcept
{
    return static_cast<double>(start) + (beat - beatStart) / beatsPerSample();
}

} // namespace devildj::engine
