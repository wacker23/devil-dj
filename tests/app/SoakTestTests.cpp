// P1-20: soak test runner. The 60-minute run itself happens in the app on the target laptop (DEVLOG); here the
// runner is driven with a fake clock against the real engine, so an "hour" takes a moment.

#include "SoakTest.h"
#include "engine/AudioEngine.h"
#include "util/ProcessStats.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>

using namespace devildj;
using app::SoakTest;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

std::unique_ptr<engine::TrackBuffer> tone(double seconds, double hz)
{
    auto t = std::make_unique<engine::TrackBuffer>();
    t->sampleRate = kRate;
    const int n = static_cast<int>(seconds * kRate);
    t->audio.setSize(2, n);
    for (int i = 0; i < n; ++i)
        for (int ch = 0; ch < 2; ++ch)
            t->audio.setSample(ch, i, static_cast<float>(0.3 * std::sin(6.283185307 * hz * i / kRate)));
    return t;
}

struct Rig
{
    engine::AudioEngine engine;
    std::unique_ptr<engine::TrackBuffer> a = tone(90.0, 220.0), b = tone(90.0, 330.0);
    double clock = 1000.0;
    std::uint32_t fakeEngineXruns = 0;
    int fakeDeviceXruns = 0;
    std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
    SoakTest soak{SoakTest::Hooks{[this]
                                  {
                                      auto s = engine.readState();
                                      // Only the test's own count: the engine here runs flat out (faster or slower than
                                      // real time), so its block-time xruns mean nothing.
                                      s.xruns = fakeEngineXruns;
                                      return s;
                                  },
                                  [this]
                                  {
                                      engine::AudioDeviceStatus d;
                                      d.isOpen = true;
                                      d.typeName = "Fake";
                                      d.deviceName = "Test device";
                                      d.sampleRate = kRate;
                                      d.bufferSize = kBlock;
                                      d.bufferMs = 5.3;
                                      d.totalOutputLatencyMs = 8.0;
                                      d.xruns = fakeDeviceXruns;
                                      return d;
                                  },
                                  [this](const engine::EngineCommand& c) { engine.send(c); },
                                  [] { return util::processMemory().privateMb; }, [this] { return clock; }}};

    Rig()
    {
        engine.prepare(kRate, kBlock);
        engine.send(engine::LoadTrack{0, a.get(), 1});
        engine.send(engine::LoadTrack{1, b.get(), 2});
        process(2);
    }
    void process(int blocks)
    {
        float* outs[] = {l.data(), r.data()};
        for (int i = 0; i < blocks; ++i)
            engine.process(outs, 2, kBlock);
    }
    // Advances the fake clock by `seconds`, ticking at 10 Hz and running the engine in between (all decks
    // move at real speed relative to the clock).
    void run(double seconds)
    {
        const int ticks = static_cast<int>(seconds * 10.0);
        const int blocksPerTick = static_cast<int>(std::lround(0.1 * kRate / kBlock));
        for (int i = 0; i < ticks; ++i)
        {
            process(blocksPerTick);
            clock += 0.1;
            soak.tick();
        }
    }
    const engine::DeckState& deck(int d) { return engine.readState().decks[static_cast<std::size_t>(d)]; }
};
} // namespace

TEST_CASE("Soak test: needs a track on both decks", "[app][soak]")
{
    engine::AudioEngine engine;
    engine.prepare(kRate, kBlock);
    SoakTest soak{SoakTest::Hooks{[&engine] { return engine.readState(); }, [] { return engine::AudioDeviceStatus{}; },
                                  [&engine](const engine::EngineCommand& c) { engine.send(c); }, [] { return 0.0; },
                                  [] { return 0.0; }}};
    CHECK(soak.start({1.0, false}, false) == "Load a track on deck A and deck B first");
    CHECK_FALSE(soak.isRunning());
}

TEST_CASE("Soak test: plays both decks with key lock, keeps them going and moves the mixer", "[app][soak]")
{
    Rig rig;
    REQUIRE(rig.soak.start({3.0, true}, false).isEmpty());
    rig.run(1.0);
    for (int d = 0; d < 2; ++d)
    {
        CHECK(rig.deck(d).playing);
        CHECK(rig.deck(d).keyLock);
    }
    CHECK(rig.deck(0).tempoPercent == 3.0);
    CHECK(rig.deck(1).tempoPercent == -2.0);
    CHECK(rig.engine.readState().masterGainDb <= -100.0f); // silent

    // EQ and filter move.
    const auto before = rig.engine.readState().channels[0];
    rig.run(2.0);
    const auto after = rig.engine.readState().channels[0];
    CHECK(before.eqDb[0] != after.eqDb[0]);
    CHECK(before.filter != after.filter);

    // 90 s tracks: they jump back before the end and never stop.
    double lowest = 1e12;
    for (int i = 0; i < 100; ++i)
    {
        rig.run(1.0);
        lowest = std::min(lowest, rig.deck(0).positionSamples);
        REQUIRE(rig.deck(0).playing);
        REQUIRE(rig.deck(1).playing);
        REQUIRE_FALSE(rig.deck(0).atEnd);
    }
    CHECK(lowest < 40.0 * kRate); // it went back

    CHECK(rig.soak.statusLine().startsWith("SOAK TEST"));
}

TEST_CASE("Soak test: per-minute stats, finish, report and reset", "[app][soak]")
{
    Rig rig;
    juce::String finishedReport;
    rig.soak.onFinished = [&](const juce::String& r) { finishedReport = r; };
    REQUIRE(rig.soak.start({2.5, true}, false).isEmpty());
    rig.run(61.0);
    REQUIRE(rig.soak.minutes().size() == 1);
    CHECK(rig.soak.minutes()[0].engineXruns == 0);
    CHECK(rig.soak.minutes()[0].memoryMb > 1.0);
    rig.fakeEngineXruns = 2; // two blocks over budget in minute 2
    rig.fakeDeviceXruns = 1;
    rig.run(100.0); // past 2.5 minutes: finishes by itself
    CHECK_FALSE(rig.soak.isRunning());
    REQUIRE(rig.soak.minutes().size() == 3); // two full minutes and the last half
    CHECK(rig.soak.minutes()[1].engineXruns == 2);
    CHECK(rig.soak.minutes()[1].deviceXruns == 1);
    CHECK(rig.soak.totalXruns() == 3);
    CHECK_FALSE(rig.soak.passed());

    CHECK(finishedReport.contains("FAIL - 3 glitches"));
    CHECK(finishedReport.contains("Try a bigger buffer (512 samples)"));
    CHECK(finishedReport.contains("minute  engine xruns"));
    CHECK(finishedReport.contains("Fake / Test device, 48000 Hz, buffer 256"));

    rig.process(4);
    CHECK_FALSE(rig.deck(0).playing); // reset afterwards
    CHECK(rig.deck(0).tempoPercent == 0.0);
    CHECK(rig.engine.readState().channels[0].eqDb[0] == 0.0f);
    CHECK(rig.engine.readState().masterGainDb == 0.0f); // silent mode undone
}

TEST_CASE("Soak test: a clean run passes; stopping early still reports", "[app][soak]")
{
    Rig rig;
    REQUIRE(rig.soak.start({60.0, true}, false).isEmpty());
    rig.run(70.0);
    rig.soak.stop();
    CHECK(rig.soak.passed());
    CHECK(rig.soak.report().contains("PASS - no glitches"));
    CHECK(rig.soak.report().contains("1.2 of 60 minutes"));
    CHECK(SoakTest::suggestedBuffer(256) == 512);
    CHECK(SoakTest::suggestedBuffer(200) == 256);
    CHECK(SoakTest::suggestedBuffer(64) == 128);
}

TEST_CASE("Process memory is reported", "[util][soak]")
{
    const auto m = util::processMemory();
    CHECK(m.privateMb > 1.0);
    CHECK(m.workingSetMb > 1.0);
}
