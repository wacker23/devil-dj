#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace devildj::app
{

// Which tracks count as played for the history (P2-23): a library track that has been playing for 60 s in all
// since it was loaded (pauses don't count; it counts once per load). Message thread; the app feeds it the decks'
// state a few times a second.
class PlayTracker
{
public:
    static constexpr double kPlayedSeconds = 60.0;
    static constexpr int kDecks = 4;

    // A new track on the deck: the library's id, 0 for a file that isn't in the library (not counted).
    void loaded(int deck, std::int64_t trackId) noexcept;
    // `seconds` have passed with the deck playing or not. Returns the track's id once, the moment it reaches 60 s.
    std::optional<std::int64_t> update(int deck, bool playing, double seconds) noexcept;

    double playedSeconds(int deck) const noexcept;

private:
    struct Deck
    {
        std::int64_t trackId = 0;
        double played = 0.0;
        bool counted = false;
    };
    std::array<Deck, kDecks> decks{};
};

} // namespace devildj::app
