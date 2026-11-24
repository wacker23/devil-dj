#pragma once

// Loads a deck's stems from the stem cache (P6-04) on a background thread: engine::loadStems for the track on
// the deck, delivered on the message thread. One request per deck at a time: a newer one (another track) makes
// the older one's result be dropped, and its read stops between stems.

#include "engine/EngineIds.h"
#include "engine/StemBuffer.h"

#include <juce_events/juce_events.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>

namespace devildj::app
{

class StemDeckLoader
{
public:
    using Loaded = std::function<void(engine::DeckId deck, std::shared_ptr<const engine::TrackBuffer> track,
                                      std::shared_ptr<engine::StemBuffer> stems, double seconds)>;
    using Failed = std::function<void(engine::DeckId deck, const juce::String& error)>;
    // How results get to the message thread (default MessageManager::callAsync; tests pump their own queue).
    using Post = std::function<void(std::function<void()>)>;
    StemDeckLoader(Loaded loaded, Failed failed, Post post = {});
    ~StemDeckLoader();

    static constexpr int kDecks = 4;
    void request(engine::DeckId deck, std::shared_ptr<const engine::TrackBuffer> track, const juce::File& folder);
    void cancel(engine::DeckId deck); // a result still on its way is dropped
    bool busy(engine::DeckId deck) const noexcept;

private:
    struct Shared;
    std::shared_ptr<Shared> shared;
    juce::ThreadPool pool{juce::ThreadPoolOptions{}.withThreadName("Stem loader").withNumberOfThreads(1)};
};

} // namespace devildj::app
