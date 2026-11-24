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

TEST_CASE("Code deck: hot swap at the next cycle line - no onset doubled or missing (P5-10)", "[codebeat][scheduler][swap]")
{
    // 128 BPM, 4 beats a cycle: a cycle is 1.875 s; cycle line 2 is at 3.75 s, beat 8.
    std::vector<ScheduledEvent> events, immediate;
    std::vector<std::string> names, immediateNames;
    bool waited = false, immediateWaited = true;
    onBigStack([&]
    {
        Sim sim;
        sim.producer->onEvaluated = [&](const devildj::codebeat::EvalResult& r) { waited = r.waitsForCycle; };
        sim.start(R"(s("bd*4"))");
        sim.tickTo(2.3);                     // in cycle 1
        sim.producer->submit(R"(s("hh*8"))"); // Ctrl+Enter
        sim.tickTo(6.0);
        events = sim.events;
        for (const auto& e : events)
            names.push_back(sim.producer->soundName(e.soundIndex));

        Sim now;
        now.producer->onEvaluated = [&](const devildj::codebeat::EvalResult& r) { immediateWaited = r.waitsForCycle; };
        now.start(R"(s("bd*4"))");
        now.tickTo(2.3);
        now.producer->submit(R"(s("hh*8"))", true); // Ctrl+Shift+Enter
        now.tickTo(3.0);
        immediate = now.events;
        for (const auto& e : immediate)
            immediateNames.push_back(now.producer->soundName(e.soundIndex));
    });
    CHECK(waited);
    const double samplesPerBeat = 60.0 * 44100.0 / 128.0;
    const auto line = std::llround(8.0 * samplesPerBeat);
    // Kicks on beats 0..7 (to the line), then hats every half beat from beat 8: each exactly once, in order.
    std::vector<std::pair<std::string, std::int64_t>> expected;
    for (int b = 0; b < 8; ++b)
        expected.emplace_back("bd", std::llround(b * samplesPerBeat));
    for (int h = 0; h < 2 * 32; ++h)
    {
        const auto at = std::llround((8.0 + h * 0.5) * samplesPerBeat);
        if (static_cast<double>(at) >= 6.0 * 44100.0)
            break;
        expected.emplace_back("hh", at);
    }
    std::vector<std::pair<std::string, std::int64_t>> got;
    for (std::size_t i = 0; i < events.size(); ++i)
        if (static_cast<double>(events[i].sampleTime) < 6.0 * 44100.0 - 0.1 * 44100.0) // the last window's tail aside
            got.emplace_back(names[i], events[i].sampleTime);
    expected.resize(std::min(expected.size(), got.size()));
    REQUIRE(got.size() >= 8 + 8);
    CHECK(got == expected);
    CHECK(std::none_of(got.begin(), got.end(), [&](const auto& g) { return g.first == "hh" && g.second < line; }));
    CHECK(std::none_of(got.begin(), got.end(), [&](const auto& g) { return g.first == "bd" && g.second >= line; }));

    // Ctrl+Shift+Enter: hats from the very next window (before the line), no kick after them.
    CHECK_FALSE(immediateWaited);
    const auto firstHat = std::find(immediateNames.begin(), immediateNames.end(), "hh");
    REQUIRE(firstHat != immediateNames.end());
    const auto hatAt = immediate[static_cast<std::size_t>(firstHat - immediateNames.begin())].sampleTime;
    CHECK(hatAt < line);
    // The 100 ms already sent stays as it was; the first hat on the half-beat grid after it (0.234 s apart).
    CHECK(static_cast<double>(hatAt) <= (2.3 + 0.1 + 0.035 + 0.235) * 44100.0);
    for (std::size_t i = static_cast<std::size_t>(firstHat - immediateNames.begin()); i < immediate.size(); ++i)
        CHECK(immediateNames[i] == "hh");
}

TEST_CASE("Code deck: sounds through the sound bank - kinds, synth notes, warnings for unknown names (P5-11)", "[codebeat][scheduler][sounds]")
{
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("devildj-nosamples", "");
    folder.createDirectory();
    std::vector<ScheduledEvent> events;
    std::vector<std::string> names, warned;
    std::vector<std::pair<std::uint16_t, devildj::app::CodeSound>> installed;
    CodeDeckProducer::Status st;
    bool noneLater = false;
    onBigStack([&]
    {
        Sim sim;
        sim.producer->setSoundBank(std::make_shared<devildj::app::CodeSoundBank>(folder));
        sim.producer->onWarning = [&](const std::string& w) { warned.push_back(w); };
        sim.start(R"(stack(s("bd:1 sd"), note("c4").s("saw"), n("0 7").s("sine"), s("bdd*4")))");
        sim.tickTo(1.2); // past the sd at half a cycle (0.94 s)
        installed = sim.producer->takeNewSounds();
        sim.tickTo(4.0); // the next cycles: the same sounds, no new ones, the warning not repeated
        events = sim.events;
        for (const auto& e : events)
            names.push_back(sim.producer->soundName(e.soundIndex));
        st = sim.producer->status();
        noneLater = sim.producer->takeNewSounds().empty();
    });
    CHECK(noneLater);
    folder.deleteRecursively();

    using devildj::engine::VoiceKind;
    // Cycle 0: bd:1, sd:0 (drum synths), saw at c4 (60), sine at n 0 and 7 (60, 67); no bdd.
    std::vector<std::string> cycle0;
    for (std::size_t i = 0; i < events.size(); ++i)
        if (events[i].sampleTime < static_cast<std::int64_t>(1.875 * 44100.0))
            cycle0.push_back(names[i]);
    std::sort(cycle0.begin(), cycle0.end());
    CHECK(cycle0 == std::vector<std::string>{"bd:1", "saw", "sd:0", "sine", "sine"});
    for (std::size_t i = 0; i < events.size(); ++i)
    {
        INFO(names[i]);
        if (names[i] == "bd:1" || names[i] == "sd:0")
            CHECK(events[i].kind == VoiceKind::DrumSynth);
        else
            CHECK(events[i].kind == VoiceKind::Synth);
        if (names[i] == "saw")
            CHECK(events[i].note == 60.0f);
    }
    std::vector<float> sineNotes;
    for (std::size_t i = 0; i < events.size(); ++i)
        if (names[i] == "sine" && events[i].sampleTime < static_cast<std::int64_t>(1.875 * 44100.0))
            sineNotes.push_back(events[i].note);
    CHECK(sineNotes == std::vector<float>{60.0f, 67.0f});
    // The sounds to install, each once with what it is.
    REQUIRE(installed.size() == 4);
    CHECK(installed[0].second.kind == devildj::app::CodeSound::Kind::Drum);
    CHECK(std::count_if(installed.begin(), installed.end(), [](const auto& s) { return s.second.voice == VoiceKind::Synth; }) == 2);
    // The unknown name: one warning, its hits counted and not played.
    CHECK(warned == std::vector<std::string>{"unknown sound \"bdd\" - did you mean \"bd\"?"});
    CHECK(st.warnings == warned);
    CHECK(st.unknownSounds >= 8);
    CHECK(std::none_of(names.begin(), names.end(), [](const std::string& n) { return n.find("bdd") != std::string::npos; }));
}
