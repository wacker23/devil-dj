// P1-05: background loading, buffers handed over by pointer, memory released after replacing a track,
// and no audio glitches while a 10-minute MP3 loads.

#include "engine/AudioDeviceService.h"
#include "engine/AudioEngine.h"
#include "engine/BufferRegistry.h"
#include "engine/TrackLoader.h"
#include "util/RealtimeGuard.h"

#include "FakeAudioDevice.h"
#include "MediaFoundationEncoder.h"
#include "TestData.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using namespace devildj::engine;

namespace
{
// Stand-in for the message thread: posted functions queue up until the test pumps them.
class ManualMessageThread
{
public:
    TrackLoader::PostFunction poster()
    {
        return [this](std::function<void()> f)
        {
            std::lock_guard lock(mutex);
            queue.push_back(std::move(f));
        };
    }

    void pump()
    {
        for (;;)
        {
            std::function<void()> f;
            {
                std::lock_guard lock(mutex);
                if (queue.empty())
                    return;
                f = std::move(queue.front());
                queue.pop_front();
            }
            f();
        }
    }

    bool pumpUntil(const std::function<bool()>& done, double timeoutSeconds)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
        while (!done())
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

private:
    std::mutex mutex;
    std::deque<std::function<void()>> queue;
};

// Engine + registry + loader wired like the app, but driven by the test.
struct Rig
{
    explicit Rig(int threads = 2)
    {
        engine.prepare(48000.0, 256);
        TrackLoader::Options options;
        options.threads = threads;
        options.startHousekeepingTimer = false;
        options.post = messages.poster();
        loader = std::make_unique<TrackLoader>(engine, registry, options);
        loader->callbacks.onLoaded = [this](DeckId deck, const TrackBuffer& t)
        {
            loaded.push_back({deck, t.file.getFileName()});
            lastHash = t.contentHash;
        };
        loader->callbacks.onError = [this](DeckId deck, const juce::File& f, const juce::String& e)
        { errors.push_back({deck, f.getFileName() + ": " + e}); };
        loader->callbacks.onProgress = [this](DeckId, const juce::File&, double p) { progress.push_back(p); };
    }

    // One engine block, like the audio callback (commands are applied here).
    void processBlock()
    {
        float l[256], r[256];
        float* outs[] = {l, r};
        engine.process(outs, 2, 256);
    }

    ManualMessageThread messages;
    AudioEngine engine;
    BufferRegistry registry;
    std::unique_ptr<TrackLoader> loader;
    std::vector<std::pair<DeckId, juce::String>> loaded;
    std::vector<std::pair<DeckId, juce::String>> errors;
    std::vector<double> progress;
    juce::String lastHash;
};

juce::File formatFile(const juce::String& name)
{
    return devildj::test::generatedFile("formats/" + name);
}

bool haveSignals()
{
    return formatFile("ref.wav").existsAsFile();
}
} // namespace

TEST_CASE("TrackLoader loads a track in the background and hands it to the engine", "[engine][loader]")
{
    if (!haveSignals())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");

    Rig rig;
    rig.loader->load(0, formatFile("ref.wav"), 48000.0);
    CHECK(rig.loader->isLoading(0));
    REQUIRE(rig.messages.pumpUntil([&] { return !rig.loaded.empty() || !rig.errors.empty(); }, 30.0));
    REQUIRE(rig.errors.empty());
    CHECK(rig.loaded.front().first == 0);
    CHECK(rig.loaded.front().second == "ref.wav");
    CHECK_FALSE(rig.loader->isLoading(0));
    CHECK(rig.registry.size() == 1);

    // Progress went up to 100 %.
    REQUIRE_FALSE(rig.progress.empty());
    CHECK(rig.progress.back() == 1.0);
    CHECK(std::is_sorted(rig.progress.begin(), rig.progress.end()));

    // The engine picks it up on its next block.
    rig.processBlock();
    const auto& deck = rig.engine.readState().decks[0];
    CHECK(deck.loaded);
    CHECK(deck.bufferId != 0);
    CHECK(deck.lengthSamples == 240000); // 5 s at 48 kHz (converted from 44.1 kHz)
    CHECK(rig.lastHash.length() == 16);
}

TEST_CASE("TrackLoader frees the old buffer after a track is replaced (counter)", "[engine][loader]")
{
    if (!haveSignals())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");

    const int baseline = TrackBuffer::liveCount();
    {
        Rig rig;
        rig.loader->load(0, formatFile("ref.wav"), 0.0);
        REQUIRE(rig.messages.pumpUntil([&] { return rig.loaded.size() == 1; }, 30.0));
        rig.processBlock();
        CHECK(TrackBuffer::liveCount() == baseline + 1);

        rig.loader->load(0, formatFile("ref.flac"), 0.0); // replace deck A
        REQUIRE(rig.messages.pumpUntil([&] { return rig.loaded.size() == 2; }, 30.0));
        CHECK(TrackBuffer::liveCount() == baseline + 2); // both alive until the engine lets go of the first

        rig.processBlock();                               // engine swaps and hands back the old id
        CHECK(rig.loader->collectReleasedBuffers() == 1); // housekeeping releases it
        CHECK(TrackBuffer::liveCount() == baseline + 1);
        CHECK(rig.registry.size() == 1);

        rig.loader->load(1, formatFile("ref.aiff"), 0.0); // deck B is independent
        REQUIRE(rig.messages.pumpUntil([&] { return rig.loaded.size() == 3; }, 30.0));
        rig.processBlock();
        CHECK(rig.loader->collectReleasedBuffers() == 0);
        CHECK(TrackBuffer::liveCount() == baseline + 2);

        rig.loader->unload(0);
        rig.processBlock();
        CHECK(rig.loader->collectReleasedBuffers() == 1);
        CHECK(TrackBuffer::liveCount() == baseline + 1);
        CHECK_FALSE(rig.engine.readState().decks[0].loaded);
        CHECK(rig.engine.readState().decks[1].loaded);
    }
    // Rig gone: registry destroyed with it, nothing left.
    CHECK(TrackBuffer::liveCount() == baseline);
}

TEST_CASE("TrackLoader: a newer load on the same deck supersedes the older one", "[engine][loader]")
{
    if (!haveSignals())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");
    const auto longMp3 = formatFile("long_10min.mp3");
    if (!longMp3.existsAsFile() || !devildj::test::mediaFoundationAvailable())
        SKIP("Needs the 10-minute MP3 and Media Foundation");

    const int baseline = TrackBuffer::liveCount();
    {
        Rig rig;
        rig.loader->load(0, longMp3, 48000.0);
        rig.loader->load(0, formatFile("ref.wav"), 48000.0); // changes its mind straight away
        REQUIRE(rig.messages.pumpUntil([&] { return !rig.loaded.empty(); }, 60.0));
        rig.messages.pumpUntil([] { return false; }, 0.5); // let the cancelled job finish and post
        rig.messages.pump();

        REQUIRE(rig.loaded.size() == 1);
        CHECK(rig.loaded.front().second == "ref.wav");
        CHECK(rig.registry.size() == 1);
    }
    CHECK(TrackBuffer::liveCount() == baseline);
}

TEST_CASE("TrackLoader reports errors", "[engine][loader]")
{
    Rig rig;
    rig.loader->load(2, juce::File("C:/no/such/track.wav"), 48000.0);
    REQUIRE(rig.messages.pumpUntil([&] { return !rig.errors.empty(); }, 10.0));
    CHECK(rig.errors.front().first == 2);
    CHECK(rig.errors.front().second.contains("not found"));
    CHECK(rig.registry.size() == 0);
}

TEST_CASE("TrackLoader can be destroyed while loading", "[engine][loader]")
{
    const auto longMp3 = formatFile("long_10min.mp3");
    if (!longMp3.existsAsFile() || !devildj::test::mediaFoundationAvailable())
        SKIP("Needs the 10-minute MP3 and Media Foundation");

    const int baseline = TrackBuffer::liveCount();
    {
        Rig rig;
        rig.loader->load(0, longMp3, 48000.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        rig.loader.reset();  // cancels and waits for the job
        rig.messages.pump(); // anything it posted is ignored safely
        CHECK(rig.loaded.empty());
    }
    CHECK(TrackBuffer::liveCount() == baseline);
}

TEST_CASE("TrackLoader content hash", "[engine][loader]")
{
    if (!haveSignals())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");

    const auto a = TrackLoader::contentHash(formatFile("ref.wav"));
    CHECK(a.length() == 16);
    CHECK(a == TrackLoader::contentHash(formatFile("ref.wav")));  // stable
    CHECK(a != TrackLoader::contentHash(formatFile("ref.aiff"))); // different files differ
    CHECK(TrackLoader::contentHash(juce::File("C:/no/such/file")).isEmpty());
    const auto longMp3 = formatFile("long_10min.mp3"); // > 2 MB: first + last MB
    if (longMp3.existsAsFile())
        CHECK(TrackLoader::contentHash(longMp3).length() == 16);
}

TEST_CASE("Loading a 10-minute MP3 doesn't glitch the audio callback", "[engine][loader][glitch]")
{
    const auto longMp3 = formatFile("long_10min.mp3");
    if (!longMp3.existsAsFile() || !devildj::test::mediaFoundationAvailable())
        SKIP("Needs the 10-minute MP3 and Media Foundation");

    juce::ScopedJuceInitialiser_GUI juce;
    AudioDeviceService service;
    service.addDeviceType(std::make_unique<devildj::test::FakeAudioDeviceType>("Fake", juce::StringArray{"Out"}));
    REQUIRE(service.initialise({}).isEmpty());
    REQUIRE(service.setSampleRate(48000.0).isEmpty());
    REQUIRE(service.setBufferSize(256).isEmpty());

    AudioEngine engine; // "deck B" stand-in: the test tone keeps playing the whole time
    BufferRegistry registry;
    ManualMessageThread messages;
    TrackLoader::Options options;
    options.startHousekeepingTimer = false;
    options.post = messages.poster();
    TrackLoader loader(engine, registry, options);
    bool loaded = false;
    loader.callbacks.onLoaded = [&](DeckId, const TrackBuffer& t)
    {
        loaded = true;
        CHECK(std::abs(t.durationSeconds() - 600.0) < 0.001);
    };
    juce::String error;
    loader.callbacks.onError = [&](DeckId, const juce::File&, const juce::String& e) { error = e; };

    engine.send(SetTestTone{true, 440.0f, -20.0f, Bus::Master, 0.0f});
    service.setAudioCallback(&engine);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto before = engine.readState();
    const auto guardBefore = devildj::util::realtimeViolationCount();
    const auto started = std::chrono::steady_clock::now();
    loader.load(0, longMp3, 48000.0);
    REQUIRE(messages.pumpUntil([&] { return loaded || error.isNotEmpty(); }, 120.0));
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    REQUIRE(error.isEmpty());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto after = engine.readState();
    service.setAudioCallback(nullptr);

    WARN("10-minute MP3 loaded in " << seconds
                                    << " s; audio blocks during load: " << (after.blockCounter - before.blockCounter)
                                    << ", peak callback load " << 100.0f * after.cpuLoadPeak << " %");
    CHECK(after.xruns == before.xruns);                            // no block over budget
    CHECK(devildj::util::realtimeViolationCount() == guardBefore); // no allocation on the audio thread
    CHECK(after.blockCounter - before.blockCounter > static_cast<std::uint64_t>(seconds * 48000.0 / 256 * 0.8));
    CHECK(after.cpuLoadPeak < 0.5f);
    CHECK(after.decks[0].loaded);
}
