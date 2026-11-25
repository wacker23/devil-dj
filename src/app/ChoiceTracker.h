#pragma once

// The choice log's bookkeeping (P6-11; MATCHER_SPEC §7.1), pure logic on the message thread: the app reports loads,
// play time and thumbs; this decides the outcomes and asks the callbacks to write them.
//
//   loaded(deck, impression): a track on the idle deck while the other plays. The deck's earlier impression that
//                             hasn't been mixed yet is ejected (outcome 2); the new one is added.
//   played(deck, seconds):    the deck's track played that long more; at 60 s in all it was mixed (outcome 1).
//   thumbs(up):               the latest impression whose track has played gets 3 / 4 (also over "mixed").

#include "matcher/ChoiceLog.h"

#include <array>
#include <functional>
#include <optional>

namespace devildj::app
{

class ChoiceTracker
{
public:
    static constexpr double kMixedSeconds = 60.0;

    std::function<std::optional<std::int64_t>(const matcher::Impression&)> add;
    std::function<void(std::int64_t id, matcher::Outcome outcome)> setOutcome;

    // `impression`: none when nothing is logged for this load (logging off, no context) - the earlier one on the
    // deck is still ejected.
    void loaded(int deck, std::optional<matcher::Impression> impression);
    void played(int deck, double seconds);
    // True when an impression took it.
    bool thumbs(bool up);

    std::optional<std::int64_t> pendingOn(int deck) const; // the deck's impression waiting for its outcome
    void clear() noexcept; // nothing pending (the log was deleted); the callbacks stay
    std::optional<std::int64_t> lastThumbed() const noexcept { return thumbed; }

private:
    struct Pending
    {
        std::int64_t id = 0;
        double played = 0.0;
        bool mixed = false;
        std::uint64_t order = 0;
    };
    std::array<std::optional<Pending>, 2> decks{};
    std::uint64_t counter = 0;
    std::optional<std::int64_t> thumbed;
};

} // namespace devildj::app
