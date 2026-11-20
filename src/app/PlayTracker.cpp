#include "PlayTracker.h"

namespace devildj::app
{

void PlayTracker::loaded(int deck, std::int64_t trackId) noexcept
{
    if (deck >= 0 && deck < kDecks)
        decks[static_cast<std::size_t>(deck)] = {trackId, 0.0, false};
}

std::optional<std::int64_t> PlayTracker::update(int deck, bool playing, double seconds) noexcept
{
    if (deck < 0 || deck >= kDecks)
        return std::nullopt;
    auto& d = decks[static_cast<std::size_t>(deck)];
    if (!playing || seconds <= 0.0 || d.trackId == 0 || d.counted)
        return std::nullopt;
    d.played += seconds;
    if (d.played < kPlayedSeconds)
        return std::nullopt;
    d.counted = true;
    return d.trackId;
}

double PlayTracker::playedSeconds(int deck) const noexcept
{
    return deck >= 0 && deck < kDecks ? decks[static_cast<std::size_t>(deck)].played : 0.0;
}

} // namespace devildj::app
