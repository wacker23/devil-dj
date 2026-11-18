#pragma once

#include "engine/AudioEngine.h"
#include "engine/BufferRegistry.h"
#include "engine/EngineIds.h"
#include "engine/TrackBuffer.h"

#include <juce_events/juce_events.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>

namespace devildj::engine
{

// Loads tracks onto decks in the background (P1-05; ARCHITECTURE §1 "Track loader pool (2)").
//
//   load(deck, file) --> pool thread (low priority): decode + resample + content hash
//                    --> message thread: BufferRegistry::add, AudioEngine::send(LoadTrack)
//   engine replaces/unloads a buffer --> AudioEngine::popReleasedBuffer --> housekeeping (10 Hz) releases it
//
// Message thread only (except where noted). A newer load() on the same deck cancels and supersedes the older one.
class TrackLoader : private juce::Timer
{
public:
    // Runs a function on the message thread. Default: juce::MessageManager::callAsync. Tests pass their own.
    using PostFunction = std::function<void(std::function<void()>)>;

    struct Options
    {
        int threads = 2;
        bool startHousekeepingTimer = true; // tests call collectReleasedBuffers() themselves
        PostFunction post;
    };

    struct Callbacks
    {
        std::function<void(DeckId, const juce::File&, double fraction)> onProgress;
        std::function<void(DeckId, const TrackBuffer&)> onLoaded;
        std::function<void(DeckId, const juce::File&, const juce::String& error)> onError;
    };

    TrackLoader(AudioEngine& engine, BufferRegistry& registry, Options options = {});
    ~TrackLoader() override;

    TrackLoader(const TrackLoader&) = delete;
    TrackLoader& operator=(const TrackLoader&) = delete;

    // Starts loading; returns a request id. targetSampleRate = the device rate (0 = keep the file's rate).
    std::uint64_t load(DeckId deck, const juce::File& file, double targetSampleRate);
    void cancel(DeckId deck);
    void unload(DeckId deck);
    bool isLoading(DeckId deck) const;

    // Releases buffers the engine has handed back. Called by the 10 Hz timer; returns how many were released.
    int collectReleasedBuffers();

    // xxHash3 of the file size + first and last 1 MB, as 16 hex digits (also used by the library, P2-02).
    static juce::String contentHash(const juce::File& file);

    Callbacks callbacks;

private:
    class LoadJob;
    struct DeckRequest
    {
        std::uint64_t id = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
    };

    void timerCallback() override;
    void deliver(DeckId deck, std::uint64_t requestId, juce::File file, std::shared_ptr<TrackBuffer> track,
                 juce::String error);

    AudioEngine& engine;
    BufferRegistry& registry;
    PostFunction post;
    juce::ThreadPool pool;
    std::array<DeckRequest, kMaxDecks> requests{};
    std::uint64_t nextRequestId = 1;
    std::shared_ptr<int> aliveToken = std::make_shared<int>(0); // posted callbacks check it is still alive
};

} // namespace devildj::engine
