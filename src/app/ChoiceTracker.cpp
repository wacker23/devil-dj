#include "app/ChoiceTracker.h"

namespace devildj::app
{

void ChoiceTracker::loaded(int deck, std::optional<matcher::Impression> impression)
{
    if (deck < 0 || deck > 1)
        return;
    auto& slot = decks[static_cast<std::size_t>(deck)];
    if (slot && !slot->mixed && setOutcome) // replaced before it was mixed
        setOutcome(slot->id, matcher::Outcome::Ejected);
    slot.reset();
    if (!impression || !add)
        return;
    if (const auto id = add(*impression))
        slot = Pending{*id, 0.0, false, ++counter};
}

void ChoiceTracker::played(int deck, double seconds)
{
    if (deck < 0 || deck > 1 || seconds <= 0.0)
        return;
    auto& slot = decks[static_cast<std::size_t>(deck)];
    if (!slot)
        return;
    slot->played += seconds;
    if (!slot->mixed && slot->played >= kMixedSeconds)
    {
        slot->mixed = true;
        if (setOutcome)
            setOutcome(slot->id, matcher::Outcome::Mixed);
    }
}

bool ChoiceTracker::thumbs(bool up)
{
    // The latest one that has been heard.
    const Pending* best = nullptr;
    for (const auto& d : decks)
        if (d && d->played > 0.0 && (best == nullptr || d->order > best->order))
            best = &*d;
    if (best == nullptr)
        return false;
    thumbed = best->id;
    if (setOutcome)
        setOutcome(best->id, up ? matcher::Outcome::ThumbsUp : matcher::Outcome::ThumbsDown);
    for (auto& d : decks) // a thumbed one isn't ejected later
        if (d && d->id == best->id)
            d->mixed = true;
    return true;
}

void ChoiceTracker::clear() noexcept
{
    decks = {};
    thumbed.reset();
}

std::optional<std::int64_t> ChoiceTracker::pendingOn(int deck) const
{
    if (deck < 0 || deck > 1 || !decks[static_cast<std::size_t>(deck)])
        return std::nullopt;
    return decks[static_cast<std::size_t>(deck)]->id;
}

} // namespace devildj::app
