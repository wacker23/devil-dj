// P4-02: the Beat Maker event source. Done when: event-timing tests at 120/174 BPM with swing.

#include "beatmaker/Model.h"
#include "beatmaker/Sequencer.h"
#include "engine/Scheduler.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>
#include <vector>

using namespace devildj;
using namespace devildj::beatmaker;
using util::Fraction;

namespace
{
constexpr double kRate = 48000.0;

// The nearest sample to `beat` at `bpm` (beat 0 = sample 0), in exact integer arithmetic.
std::int64_t exactSample(Fraction beat, int bpm)
{
    // beat x 60 x 48000 / bpm, rounded half up.
    const std::int64_t num = beat.numerator() * 2880000, den = beat.denominator() * bpm;
    return (2 * num + den) / (2 * den);
}

// What a producer would do: every 25 ms, the window up to 100 ms ahead, events to samples.
struct Run
{
    std::vector<BeatEvent> beats;
    std::vector<engine::ScheduledEvent> events;
};

Run produce(const Project& project, const Sequencer& seq, double bpm, double untilBeat)
{
    Run run;
    engine::ClockSnapshot clock{0, 0.0, bpm, kRate};
    engine::ScheduleCursor cursor;
    for (std::int64_t now = 0; clock.beatAt(static_cast<double>(now)) < untilBeat + 1.0; now += 1200)
    {
        const auto w = cursor.next(clock.beatAt(static_cast<double>(now)), clock.beatAt(static_cast<double>(now + 4800)));
        std::vector<BeatEvent> got;
        seq.emit(project, w.begin, w.end, got);
        for (const auto& e : got)
        {
            run.beats.push_back(e);
            run.events.push_back(toScheduled(e, clock));
            CHECK(run.events.back().sampleTime >= now); // never in the past when it is pushed
        }
    }
    return run;
}

Project allSteps(float swing, bool withMicro)
{
    Project p;
    p.swing = swing;
    auto& a = p.patterns[0];
    a.drums.push_back(drumTrack("closedHat", 16));
    const std::int8_t micro[] = {0, 50, -50, 25};
    for (int k = 0; k < 16; ++k)
    {
        a.drums[0].steps[static_cast<std::size_t>(k)].on = true;
        if (withMicro)
            a.drums[0].steps[static_cast<std::size_t>(k)].microTiming = micro[k % 4];
    }
    return p;
}
} // namespace

TEST_CASE("Beat Maker sequencer: every step at its exact sample at 90 / 120 / 128 / 174 BPM, swing 0 and 0.5, "
          "micro-timing +-50 %",
          "[beatmaker][sequencer]")
{
    for (const int bpm : {90, 120, 128, 174})
        for (const float swing : {0.0f, 0.5f})
            for (const bool micro : {false, true})
            {
                const auto project = allSteps(swing, micro);
                Sequencer seq;
                seq.playPattern(0, 0);
                const auto run = produce(project, seq, bpm, 32.0);
                // The expected hits in beats [0, 32): pass n, step k.
                std::vector<std::int64_t> expected, got;
                const std::int8_t microOf[] = {0, 50, -50, 25};
                for (int n = 0; n < 9; ++n)
                    for (int k = 0; k < 16; ++k)
                    {
                        Fraction beat = Fraction(4 * n) + Fraction(k, 4);
                        if (k % 2 == 1)
                            beat += Fraction::fromDouble(swing, 1000) * Fraction(1, 8);
                        if (micro)
                            beat += Fraction(microOf[k % 4], 400);
                        if (beat >= Fraction(0) && beat < Fraction(32))
                            expected.push_back(exactSample(beat, bpm));
                    }
                for (std::size_t i = 0; i < run.beats.size(); ++i)
                    if (run.beats[i].beat < Fraction(32))
                        got.push_back(run.events[i].sampleTime);
                CHECK(std::is_sorted(got.begin(), got.end()));
                std::sort(expected.begin(), expected.end());
                INFO(bpm << " BPM, swing " << swing << (micro ? ", micro-timing" : ""));
                CHECK(got == expected); // none missing, none doubled, each on its sample
            }
    // The figures themselves at 120 BPM (a 16th = 6000 samples): swing 0.5 moves the odd steps 1500 later.
    const auto run = produce(allSteps(0.5f, false), [] { Sequencer s; s.playPattern(0, 0); return s; }(), 120, 4.0);
    REQUIRE(run.events.size() >= 4);
    CHECK(run.events[0].sampleTime == 0);
    CHECK(run.events[1].sampleTime == 7500);
    CHECK(run.events[2].sampleTime == 12000);
    CHECK(run.events[3].sampleTime == 19500);
}

TEST_CASE("Beat Maker sequencer: windows of any size give the same events", "[beatmaker][sequencer]")
{
    auto project = allSteps(0.3f, true);
    project.patterns[0].drums[0].steps[5].ratchet = 3;
    project.patterns[0].drums[0].steps[9].probability = 50;
    SynthTrack synth;
    synth.notes = {{0, 48, 40, 100}, {96, 24, 43, 90}, {200, 30, 47, 80}};
    project.patterns[0].synths.push_back(synth);
    Sequencer seq;
    seq.playPattern(0, Fraction(1, 2));
    std::vector<BeatEvent> whole, pieces;
    seq.emit(project, 0, 40, whole);
    std::mt19937 rng(9);
    Fraction at = 0;
    while (at < Fraction(40))
    {
        Fraction next = at + Fraction(std::uniform_int_distribution<int>(1, 997)(rng), 2000);
        if (next > Fraction(40))
            next = 40;
        seq.emit(project, at, next, pieces);
        at = next;
    }
    REQUIRE(whole.size() == pieces.size());
    for (std::size_t i = 0; i < whole.size(); ++i)
    {
        CHECK(whole[i].beat == pieces[i].beat);
        CHECK(whole[i].track == pieces[i].track);
        CHECK(whole[i].kind == pieces[i].kind);
    }
    CHECK(whole.front().beat >= Fraction(1, 2)); // nothing before the pattern starts
}

TEST_CASE("Beat Maker sequencer: probability is the same every time for a seed", "[beatmaker][sequencer]")
{
    Project p;
    p.patterns[0].drums.push_back(drumTrack("kick", 16));
    for (auto& s : p.patterns[0].drums[0].steps)
    {
        s.on = true;
        s.probability = 50;
    }
    Sequencer seq;
    seq.playPattern(0, 0);
    const auto hits = [&](std::uint32_t seed)
    {
        p.seed = seed;
        std::vector<BeatEvent> out;
        seq.emit(p, 0, 400, out); // 100 loops, 1600 steps
        std::vector<Fraction> beats;
        for (const auto& e : out)
            beats.push_back(e.beat);
        return beats;
    };
    const auto a = hits(1), b = hits(1), c = hits(2);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a.size() > 700);
    CHECK(a.size() < 900);
    // 0 % never, 100 % always.
    for (auto& s : p.patterns[0].drums[0].steps)
        s.probability = 0;
    CHECK(hits(1).empty());
    for (auto& s : p.patterns[0].drums[0].steps)
        s.probability = 100;
    CHECK(hits(1).size() == 1600);
    CHECK(Sequencer::chance(1, 0, 0, 3, 7) == Sequencer::chance(1, 0, 0, 3, 7));
}

TEST_CASE("Beat Maker sequencer: ratchets, velocity, solo and mute, the track's mix", "[beatmaker][sequencer]")
{
    Project p;
    auto& a = p.patterns[0];
    a.drums.push_back(drumTrack("snare", 16));
    a.drums.push_back(drumTrack("kick", 16));
    a.drums[0].steps[4] = {true, 127, 100, 0, 4};
    a.drums[0].volumeDb = -6.0f;
    a.drums[0].pan = 0.25f;
    a.drums[1].steps[0].on = true;
    Sequencer seq;
    seq.playPattern(0, 0);
    std::vector<BeatEvent> out;
    seq.emit(p, 0, 4, out);
    REQUIRE(out.size() == 5);
    CHECK(out[0].track == 1);
    for (int h = 0; h < 4; ++h)
    {
        const auto& e = out[static_cast<std::size_t>(h + 1)];
        CHECK(e.beat == Fraction(1) + Fraction(h, 16)); // a 16th split in 4
        CHECK(std::abs(e.velocity - std::pow(0.9f, static_cast<float>(h))) < 1e-5f);
        CHECK(std::abs(e.gain - 0.501187f) < 1e-4f);
        CHECK(e.pan == 0.25f);
    }
    // Solo the kick: only it. Mute it: only the snare.
    a.drums[1].solo = true;
    out.clear();
    seq.emit(p, 0, 4, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].track == 1);
    a.drums[1].solo = false;
    a.drums[1].mute = true;
    out.clear();
    seq.emit(p, 0, 4, out);
    CHECK(out.size() == 4);
    // To the voices: its sample, its length in seconds, where it came from.
    const engine::ClockSnapshot clock{48000, 1.0, 120.0, kRate};
    const auto s = toScheduled(out[0], clock);
    CHECK(s.sampleTime == 48000);
    CHECK(s.source == engine::SourceId::BeatMaker);
    CHECK(s.soundIndex == 0);
    CHECK(std::abs(s.durationSeconds - 0.03125f) < 1e-6f);
    CHECK(s.highlightToken == 4u);
}

TEST_CASE("Beat Maker sequencer: synth notes and the arpeggiator", "[beatmaker][sequencer]")
{
    Project p;
    auto& a = p.patterns[0];
    SynthTrack s;
    s.notes = {{0, 96, 60, 100}, {0, 96, 64, 100}, {0, 96, 67, 100}, {192, 48, 72, 50}};
    a.synths.push_back(s);
    Sequencer seq;
    seq.playPattern(0, 0);
    std::vector<BeatEvent> out;
    seq.emit(p, 0, 4, out);
    REQUIRE(out.size() == 4);
    CHECK(out[3].beat == Fraction(2));
    CHECK(out[3].duration == Fraction(1, 2));
    CHECK(out[3].pitch == 72);
    const auto pitches = [&](ArpSettings::Mode mode, int octaves)
    {
        a.synths[0].arp = {};
        a.synths[0].arp.on = true;
        a.synths[0].arp.mode = mode;
        a.synths[0].arp.octaves = octaves;
        out.clear();
        seq.emit(p, 0, 1, out); // the chord's beat: four 16ths
        std::vector<int> v;
        for (const auto& e : out)
            v.push_back(e.pitch);
        return v;
    };
    CHECK(pitches(ArpSettings::Mode::Up, 1) == std::vector<int>{60, 64, 67, 60});
    CHECK(pitches(ArpSettings::Mode::Down, 1) == std::vector<int>{67, 64, 60, 67});
    CHECK(pitches(ArpSettings::Mode::UpDown, 1) == std::vector<int>{60, 64, 67, 64});
    CHECK(pitches(ArpSettings::Mode::Up, 2) == std::vector<int>{60, 64, 67, 72});
    a.synths[0].arp.rate = 8;
    out.clear();
    seq.emit(p, 0, 1, out);
    CHECK(out.size() == 2);
    // The 4th note alone, later: the arp starts its order again.
    a.synths[0].arp.rate = 16;
    a.synths[0].arp.octaves = 1;
    out.clear();
    seq.emit(p, 2, 3, out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].pitch == 72);
    CHECK(out[1].pitch == 72);
}

TEST_CASE("Beat Maker sequencer: song mode plays the pattern list round and round", "[beatmaker][sequencer]")
{
    Project p;
    for (int i = 0; i < 3; ++i)
    {
        auto& pat = p.patterns[static_cast<std::size_t>(i)];
        pat.drums.push_back(drumTrack("kick", 16));
        pat.drums[0].steps[0].on = true;
    }
    p.patterns[1].lengthSteps = 32; // 8 beats
    setPatternLength(p.patterns[1], 32);
    p.song = {0, 1, 2};
    Sequencer seq;
    seq.playSong(0);
    std::vector<BeatEvent> out;
    seq.emit(p, 0, 32, out);
    std::vector<std::pair<int, Fraction>> got;
    for (const auto& e : out)
        got.push_back({e.pattern, e.beat});
    const std::vector<std::pair<int, Fraction>> expected{{0, 0}, {1, 4}, {2, 12}, {0, 16}, {1, 20}, {2, 28}};
    CHECK(got == expected);
    // An empty song: the current pattern loops.
    p.song.clear();
    seq.playPattern(2, 0);
    seq.playSong(0);
    out.clear();
    seq.emit(p, 0, 8, out);
    REQUIRE(out.size() == 2);
    CHECK(out[1].pattern == 2);
}

TEST_CASE("Scheduler: the cursor's windows join up, re-anchor after a jump; the event buffer sorts and drops late ones",
          "[engine][scheduler]")
{
    engine::ScheduleCursor cursor;
    auto w = cursor.next(10.0, 10.2);
    CHECK(w.reanchored);
    CHECK(w.begin == Fraction(10));
    const auto end = w.end;
    w = cursor.next(10.05, 10.25);
    CHECK_FALSE(w.reanchored);
    CHECK(w.begin == end);
    w = cursor.next(40.0, 40.2); // the clock jumped
    CHECK(w.reanchored);
    CHECK(w.begin == Fraction(40));

    engine::EventBuffer<8> buffer;
    for (const std::int64_t t : {500, 100, 300, 100, 900})
    {
        engine::ScheduledEvent e;
        e.sampleTime = t;
        e.soundIndex = static_cast<std::uint16_t>(t / 100);
        buffer.insert(e);
    }
    std::vector<std::pair<std::int64_t, int>> due;
    buffer.takeDue(200, 400, [&](const engine::ScheduledEvent& e, int offset) { due.push_back({e.sampleTime, offset}); });
    CHECK(buffer.lateEvents() == 2); // the two at 100
    CHECK(due == std::vector<std::pair<std::int64_t, int>>{{300, 100}, {500, 300}});
    CHECK(buffer.size() == 1);
    for (int i = 0; i < 10; ++i)
        buffer.insert({});
    CHECK(buffer.size() == 8);
    CHECK(buffer.overflowEvents() == 3);
    engine::EventQueue queue;
    engine::ScheduledEvent e;
    e.sampleTime = 42;
    queue.tryPush(e);
    buffer.clear();
    buffer.drain(queue);
    CHECK(buffer.size() == 1);
}
