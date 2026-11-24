// P5-09: the code deck's producer - pattern onsets to engine samples, exactly, over a long run.

#include "app/CodeDeckProducer.h"
#include "codebeat/BigStack.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <vector>

using devildj::app::CodeDeckProducer;
using devildj::engine::ScheduledEvent;
using devildj::testing::onBigStack;

namespace
{
// A producer ticked by hand on simulated time: ticks every 25 ms plus a deterministic 0..8 ms of jitter.
struct Sim
{
    std::vector<ScheduledEvent> events;
    std::vector<std::int64_t> nowAtPush; // the sample "now" when each event was made
    double t = 0.0;
    std::int64_t sampleNow = 0;
    std::uint32_t rng = 12345;
    double bpm = 128.0, sampleRate = 44100.0;
    std::unique_ptr<CodeDeckProducer> producer;

    Sim()
    {
        producer = std::make_unique<CodeDeckProducer>(
            [this](const ScheduledEvent& e)
            {
                events.push_back(e);
                nowAtPush.push_back(sampleNow);
                return true;
            },
            false);
        producer->now = [this] { return t; };
    }

    void start(const std::string& code)
    {
        producer->setClock({0, 0.0, bpm, sampleRate, 0.0});
        producer->submit(code);
    }

    double jitter()
    {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<double>(rng >> 8) / static_cast<double>(1u << 24) * 0.008;
    }

    void tickTo(double until)
    {
        while (t < until)
        {
            sampleNow = static_cast<std::int64_t>(t * sampleRate);
            producer->tick();
            t += 0.025 + jitter();
        }
    }
};
} // namespace

TEST_CASE("Code deck: a kick on every beat lands within 1 sample at 128 BPM for 10 minutes", "[codebeat][scheduler]")
{
    std::vector<ScheduledEvent> events;
    CodeDeckProducer::Status st;
    double samplesPerBeat = 0.0;
    onBigStack([&]
    {
        Sim sim;
        sim.start(R"(s("bd*4"))");
        sim.tickTo(600.0);
        events = sim.events;
        st = sim.producer->status();
        samplesPerBeat = 60.0 * sim.sampleRate / sim.bpm; // 20671.875: not a whole number of samples
    });
    INFO(st.lastError);
    REQUIRE(st.ready);
    REQUIRE(events.size() > 1000);

    // Beat k is due at round(k * 20671.875): every beat from the first one to the last, once each.
    const auto first = static_cast<std::int64_t>(std::llround(static_cast<double>(events.front().sampleTime) / samplesPerBeat));
    std::int64_t worst = 0;
    std::int64_t missingOrDoubled = 0;
    for (std::size_t i = 0; i < events.size(); ++i)
    {
        const auto beat = first + static_cast<std::int64_t>(i);
        const auto expected = std::llround(static_cast<double>(beat) * samplesPerBeat);
        worst = std::max(worst, std::abs(events[i].sampleTime - expected));
        if (i > 0 && std::abs(events[i].sampleTime - events[i - 1].sampleTime - static_cast<std::int64_t>(samplesPerBeat)) > 1)
            ++missingOrDoubled;
    }
    CHECK(worst <= 1);
    CHECK(missingOrDoubled == 0);
    // 10 minutes at 128 BPM: 1280 beats (the first may fall before the deck's first window).
    CHECK(events.size() >= 1279);
    CHECK(events.size() <= 1281);
    CHECK(st.late == 0);
    CHECK(st.reanchors == 0);
    CHECK(st.slowWindows == 0);
    CHECK(st.pushed == events.size());
    // Events are made ahead of time, at most the look-ahead plus one tick.
    CHECK(st.minLeadMs >= 0.0);
    CHECK(st.maxLeadMs <= 140.0);
    WARN("10 min at 128 BPM: " << events.size() << " kicks, worst error " << worst << " samples; ticks " << st.ticks
         << " (mean " << st.meanTickMs << " ms, max " << st.maxTickMs << " ms); lead " << st.minLeadMs << ".."
         << st.maxLeadMs << " ms");
    // Every event is the code deck's, with its sound.
    CHECK(std::all_of(events.begin(), events.end(), [](const ScheduledEvent& e) { return e.source == devildj::engine::SourceId::CodeDeck; }));
}

TEST_CASE("Code deck: a held-up thread drops late events (counted); a jump re-anchors", "[codebeat][scheduler]")
{
    std::vector<ScheduledEvent> events;
    std::vector<std::int64_t> madeAt;
    CodeDeckProducer::Status afterGap, afterJump;
    onBigStack([&]
    {
        Sim sim;
        sim.start(R"(s("hh*32"))"); // a hat every 58 ms
        sim.tickTo(2.0);
        sim.t += 0.2; // the thread was held up for 200 ms: the window behind now has passed
        sim.tickTo(4.0);
        afterGap = sim.producer->status();
        sim.t += 1.5; // a long stall: the cursor is too far behind and re-anchors at now
        sim.tickTo(6.0);
        afterJump = sim.producer->status();
        events = sim.events;
        madeAt = sim.nowAtPush;
    });
    CHECK(afterGap.late >= 1);
    CHECK(afterGap.reanchors == 0);
    CHECK(afterJump.reanchors == 1);
    // Nothing was pushed for a moment already past, and nothing twice.
    for (std::size_t i = 0; i < events.size(); ++i)
    {
        CHECK(events[i].sampleTime >= madeAt[i]);
        if (i > 0)
            CHECK(events[i].sampleTime > events[i - 1].sampleTime);
    }
}

TEST_CASE("Code deck: controls, sounds and setcpb reach the events", "[codebeat][scheduler]")
{
    std::vector<ScheduledEvent> events;
    std::string soundA, soundB;
    std::vector<ScheduledEvent> threeBeat;
    onBigStack([&]
    {
        Sim sim;
        sim.start(R"(stack(s("bd:3").gain(0.5).pan(0.25).lpf(800).hpf(120).cut(2).legato(0.5).room(0.3).delay(0.2).crush(8),
                            note("a4").s("saw").attack(0.01).release(0.4).speed(-1)))");
        sim.tickTo(0.5);
        events = sim.events;
        if (!events.empty())
            soundA = sim.producer->soundName(events.front().soundIndex);
        if (events.size() > 1)
            soundB = sim.producer->soundName(events[1].soundIndex);

        Sim three;
        three.start("setcpb(3);\ns(\"bd*3\")"); // 3 beats per cycle: a kick on each beat
        three.tickTo(5.0);
        threeBeat = three.events;
    });
    REQUIRE(events.size() == 2);
    const auto& bd = events[0].soundIndex == events[1].soundIndex ? events[0] : (soundA == "bd:3" ? events[0] : events[1]);
    const auto& saw = &bd == &events[0] ? events[1] : events[0];
    CHECK((soundA == "bd:3" || soundB == "bd:3"));
    CHECK((soundA == "saw" || soundB == "saw"));
    CHECK(bd.sampleTime == 0);
    CHECK(bd.gain == 0.5f);
    CHECK(bd.pan == 0.25f);
    CHECK(bd.cutoffHz == 800.0f);
    CHECK(bd.hpfHz == 120.0f);
    CHECK(bd.chokeGroup == 2);
    CHECK(bd.roomSend == 0.3f);
    CHECK(bd.delaySend == 0.2f);
    CHECK(bd.crush == 8.0f);
    // One cycle = 4 beats at 128 BPM = 1.875 s; legato 0.5 halves it.
    CHECK(std::abs(bd.durationSeconds - 0.9375f) < 1e-5f);
    CHECK(saw.note == 69.0f);
    CHECK(saw.attack == 0.01f);
    CHECK(saw.release == 0.4f);
    CHECK(saw.speed == -1.0f);
    CHECK(std::abs(saw.durationSeconds - 1.875f) < 1e-5f);

    // setcpb(3): bd*3 is one kick per beat.
    REQUIRE(threeBeat.size() >= 9);
    for (std::size_t i = 1; i < threeBeat.size(); ++i)
        CHECK(std::abs(threeBeat[i].sampleTime - threeBeat[i - 1].sampleTime - 20672) <= 1);
}

TEST_CASE("Code deck: bad code keeps the last pattern; hush stops; nothing before any code", "[codebeat][scheduler]")
{
    std::size_t silentStart = 1, afterGood = 0, afterBad = 0, afterHush = 0;
    std::string error;
    onBigStack([&]
    {
        Sim sim;
        sim.producer->setClock({0, 0.0, 128.0, 44100.0, 0.0});
        sim.tickTo(1.0);
        silentStart = sim.events.size();
        sim.producer->submit(R"(s("bd*4"))");
        sim.tickTo(3.0);
        afterGood = sim.events.size();
        sim.producer->submit(R"js(s("bd [sd"))js");
        sim.tickTo(5.0);
        afterBad = sim.events.size();
        error = sim.producer->status().lastError;
        sim.producer->submit("hush()");
        sim.tickTo(5.1);
        const auto before = sim.events.size();
        sim.tickTo(8.0);
        afterHush = sim.events.size() - before;
    });
    CHECK(silentStart == 0);
    CHECK(afterGood >= 4);
    CHECK(afterBad >= afterGood + 4); // still playing
    CHECK(error.find("missing \"]\"") != std::string::npos);
    CHECK(afterHush == 0);
}

TEST_CASE("Code deck: on its own thread in real time (jitter logged)", "[codebeat][scheduler]")
{
    std::vector<ScheduledEvent> events;
    std::mutex m;
    CodeDeckProducer::Status st;
    {
        CodeDeckProducer producer([&](const ScheduledEvent& e)
                                  {
                                      std::lock_guard<std::mutex> lock(m);
                                      events.push_back(e);
                                      return true;
                                  });
        const double t0 = producer.now();
        producer.setClock({0, 0.0, 128.0, 48000.0, t0});
        producer.submit(R"(s("hh*16"))");
        juce::Thread::sleep(1500);
        st = producer.status();
    }
    INFO(st.lastError);
    CHECK(st.ready);
    CHECK(st.ticks >= 30);
    CHECK(st.pushed > 0);
    CHECK(st.meanTickMs > 20.0);
    CHECK(st.meanTickMs < 60.0);
    WARN("real time 1.5 s: " << st.pushed << " events, " << st.late << " late, ticks " << st.ticks << " (mean "
         << st.meanTickMs << " ms, max " << st.maxTickMs << " ms), lead " << st.minLeadMs << ".." << st.maxLeadMs
         << " ms, slow windows " << st.slowWindows);
    std::lock_guard<std::mutex> lock(m);
    for (std::size_t i = 1; i < events.size(); ++i)
        CHECK(events[i].sampleTime > events[i - 1].sampleTime);
}
