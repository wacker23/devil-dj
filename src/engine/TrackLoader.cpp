#include "engine/TrackLoader.h"

#include "analysis/WaveformBuilder.h"
#include "library/TagReader.h"

#include "engine/TrackDecoder.h"
#include "util/Log.h"

#include <xxhash.h>

namespace devildj::engine
{

namespace
{
constexpr int kHousekeepingHz = 10;
constexpr double kProgressStep = 0.05; // post progress at most every 5 %
constexpr std::int64_t kHashBytes = 1 << 20;
} // namespace

class TrackLoader::LoadJob final : public juce::ThreadPoolJob
{
public:
    LoadJob(DeckId deckId, std::uint64_t request, juce::File fileToLoad, double rate,
            std::shared_ptr<std::atomic<bool>> cancelFlag, PostFunction postFn, std::weak_ptr<int> alive,
            TrackLoader* owner)
        : ThreadPoolJob("Load " + fileToLoad.getFileName())
        , deck(deckId)
        , requestId(request)
        , file(std::move(fileToLoad))
        , targetRate(rate)
        , cancel(std::move(cancelFlag))
        , post(std::move(postFn))
        , loaderAlive(std::move(alive))
        , loader(owner)
    {
    }

    JobStatus runJob() override
    {
        const auto started = juce::Time::getMillisecondCounterHiRes();
        DecodeOptions options;
        options.targetSampleRate = targetRate;
        options.cancel = cancel.get();
        double lastPosted = -1.0;
        options.progress = [this, &lastPosted](double fraction)
        {
            if (fraction - lastPosted < kProgressStep && fraction < 1.0)
                return;
            lastPosted = fraction;
            postToLoader(
                [deck = deck, file = file, fraction](TrackLoader& l)
                {
                    if (l.callbacks.onProgress)
                        l.callbacks.onProgress(deck, file, fraction);
                });
        };

        auto result = decodeTrack(file, options);
        std::shared_ptr<TrackBuffer> track;
        juce::String error;
        if (result)
        {
            track = std::move(result).value();
            track->contentHash = contentHash(file);
            const auto tags = library::readBasicTags(file.getFullPathName().toWideCharPointer());
            track->title = juce::String::fromUTF8(tags.title.c_str());
            track->artist = juce::String::fromUTF8(tags.artist.c_str());
            track->tagBpm = tags.bpm;
            track->waveform =
                analysis::buildWaveform(track->audio.getReadPointer(0),
                                        track->audio.getReadPointer(std::min(1, track->audio.getNumChannels() - 1)),
                                        track->numSamples(), track->sampleRate);
            util::logInfo("Loader: {} decoded in {:.0f} ms ({}, {:.1f} s, {:.0f} -> {:.0f} Hz)",
                          file.getFileName().toStdString(), juce::Time::getMillisecondCounterHiRes() - started,
                          track->formatName.toStdString(), track->durationSeconds(), track->sourceSampleRate,
                          track->sampleRate);
        }
        else
        {
            error = juce::String(result.error());
        }

        postToLoader([deck = deck, requestId = requestId, file = file, track, error](TrackLoader& l) mutable
                     { l.deliver(deck, requestId, std::move(file), std::move(track), std::move(error)); });
        return jobHasFinished;
    }

private:
    template <typename F>
    void postToLoader(F&& f)
    {
        post(
            [alive = loaderAlive, owner = loader, fn = std::forward<F>(f)]() mutable
            {
                if (!alive.expired()) // the loader may have been destroyed in the meantime
                    fn(*owner);
            });
    }

    DeckId deck;
    std::uint64_t requestId;
    juce::File file;
    double targetRate;
    std::shared_ptr<std::atomic<bool>> cancel;
    PostFunction post;
    std::weak_ptr<int> loaderAlive;
    TrackLoader* loader;
};

TrackLoader::TrackLoader(AudioEngine& audioEngine, BufferRegistry& bufferRegistry, Options options)
    : engine(audioEngine)
    , registry(bufferRegistry)
    , post(options.post ? std::move(options.post)
                        : PostFunction([](std::function<void()> f) { juce::MessageManager::callAsync(std::move(f)); }))
    , pool(juce::ThreadPoolOptions{}
               .withThreadName("Track loader")
               .withNumberOfThreads(std::max(1, options.threads))
               .withDesiredThreadPriority(juce::Thread::Priority::low))
{
    if (options.startHousekeepingTimer)
        startTimerHz(kHousekeepingHz);
}

TrackLoader::~TrackLoader()
{
    stopTimer();
    for (auto& r : requests)
        if (r.cancel)
            r.cancel->store(true);
    pool.removeAllJobs(true, 10'000);
    aliveToken.reset(); // results still queued for the message thread are dropped
}

std::uint64_t TrackLoader::load(DeckId deck, const juce::File& file, double targetSampleRate)
{
    jassert(deck < kMaxDecks);
    cancel(deck);

    auto& r = requests[deck];
    r.id = nextRequestId++;
    r.cancel = std::make_shared<std::atomic<bool>>(false);
    pool.addJob(new LoadJob(deck, r.id, file, targetSampleRate, r.cancel, post, aliveToken, this), true);
    util::logInfo("Loader: deck {} <- {}", deckLetter(deck), file.getFullPathName().toStdString());
    return r.id;
}

void TrackLoader::cancel(DeckId deck)
{
    auto& r = requests[deck];
    if (r.cancel)
        r.cancel->store(true);
    r = {};
}

void TrackLoader::unload(DeckId deck)
{
    cancel(deck);
    engine.send(UnloadTrack{deck});
}

bool TrackLoader::isLoading(DeckId deck) const
{
    return requests[deck].id != 0;
}

void TrackLoader::deliver(DeckId deck, std::uint64_t requestId, juce::File file, std::shared_ptr<TrackBuffer> track,
                          juce::String error)
{
    auto& r = requests[deck];
    if (r.id != requestId)
        return; // superseded or cancelled: drop the result (the buffer is freed here, on the message thread)
    r = {};

    if (track == nullptr)
    {
        if (error != "Cancelled")
            util::logWarning("Loader: deck {}: {}", deckLetter(deck), error.toStdString());
        if (callbacks.onError)
            callbacks.onError(deck, file, error);
        return;
    }

    const TrackBuffer* raw = track.get();
    const BufferId id = registry.add(std::move(track));
    if (!engine.send(LoadTrack{deck, raw, id}))
    {
        registry.release(id);
        if (callbacks.onError)
            callbacks.onError(deck, file, "Audio engine is busy; try again");
        return;
    }
    if (callbacks.onLoaded)
        callbacks.onLoaded(deck, *raw);
}

int TrackLoader::collectReleasedBuffers()
{
    int released = 0;
    BufferId id = 0;
    while (engine.popReleasedBuffer(id))
        if (registry.release(id))
            ++released;
    return released;
}

void TrackLoader::timerCallback()
{
    collectReleasedBuffers();
}

juce::String TrackLoader::contentHash(const juce::File& file)
{
    std::unique_ptr<juce::FileInputStream> in(file.createInputStream().release());
    if (in == nullptr || !in->openedOk())
        return {};

    const std::int64_t size = in->getTotalLength();
    XXH3_state_t* state = XXH3_createState();
    XXH3_64bits_reset(state);
    XXH3_64bits_update(state, &size, sizeof(size));

    juce::HeapBlock<char> buffer(static_cast<std::size_t>(kHashBytes));
    const auto hashRange = [&](std::int64_t from, std::int64_t length)
    {
        in->setPosition(from);
        const int got = in->read(buffer.get(), static_cast<int>(length));
        if (got > 0)
            XXH3_64bits_update(state, buffer.get(), static_cast<std::size_t>(got));
    };
    hashRange(0, std::min(size, kHashBytes));
    if (size > kHashBytes)
        hashRange(std::max(kHashBytes, size - kHashBytes), std::min(kHashBytes, size - kHashBytes));

    const auto digest = XXH3_64bits_digest(state);
    XXH3_freeState(state);
    return juce::String::toHexString(static_cast<juce::int64>(digest)).paddedLeft('0', 16);
}

} // namespace devildj::engine
