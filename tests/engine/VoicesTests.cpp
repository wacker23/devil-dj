// P4-03: the voices. Done when: 64 voices at 48 kHz within CPU budget; stealing tests.

#include "engine/Voices.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <vector>

using namespace devildj;
using engine::ScheduledEvent;
using engine::VoiceKind;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

struct Sample
{
    std::vector<float> left, right;
    const float* channels[2]{};
    engine::SampleData data;

    Sample(int frames, float value)
        : left(static_cast<std::size_t>(frames), value)
        , right(static_cast<std::size_t>(frames), value)
    {
        channels[0] = left.data();
        channels[1] = right.data();
        data = {channels, 2, frames, kRate};
    }
};

struct Buses
{
    std::vector<float> l, r, dl, dr, rl, rr;
    explicit Buses(int n)
        : l(n), r(n), dl(n), dr(n), rl(n), rr(n)
    {
    }
    engine::VoiceBuses get() { return {l.data(), r.data(), dl.data(), dr.data(), rl.data(), rr.data()}; }
    void clear()
    {
        for (auto* v : {&l, &r, &dl, &dr, &rl, &rr})
            std::fill(v->begin(), v->end(), 0.0f);
    }
};

ScheduledEvent hit(std::uint16_t sound, float pan = 0.5f)
{
    ScheduledEvent e;
    e.kind = VoiceKind::Sample;
    e.soundIndex = sound;
    e.pan = pan;
    return e;
}
} // namespace

TEST_CASE("Voices: a sample starts on its sample offset, ramps in, is panned, pitched and sent", "[engine][voices]")
{
    engine::Voices v;
    v.prepare(kRate, kBlock);
    Sample s(800, 1.0f);
    v.setSample(3, &s.data);
    auto e = hit(3);
    e.delaySend = 0.5f;
    e.roomSend = 0.25f;
    REQUIRE(v.start(e, 100) != 0);
    Buses b(1024);
    v.process(b.get(), 1024);
    CHECK(b.l[99] == 0.0f);
    CHECK(b.l[100] > 0.0f);
    CHECK(b.l[100] < 0.1f); // ramping in, no click
    CHECK(std::abs(b.l[300] - 0.70710678f) < 1e-4f);
    CHECK(std::abs(b.r[300] - 0.70710678f) < 1e-4f);
    CHECK(std::abs(b.dl[300] - 0.5f * b.l[300]) < 1e-6f);
    CHECK(std::abs(b.rr[300] - 0.25f * b.r[300]) < 1e-6f);
    CHECK(b.l[1023] == 0.0f); // 800 frames from 100: over at 900
    CHECK(v.activeSampleVoices() == 0);

    // An octave up: twice as fast, so half as long; panned hard left.
    auto up = hit(3, 0.0f);
    up.note = 72.0f;
    v.start(up, 0);
    b.clear();
    v.process(b.get(), 1024);
    CHECK(b.l[300] > 0.9f);
    CHECK(std::abs(b.r[300]) < 1e-6f);
    CHECK(b.l[420] == 0.0f);
    // Missing samples and drum synths (until P4-04) don't start.
    CHECK(v.start(hit(9), 0) == 0);
    auto drum = hit(3);
    drum.kind = VoiceKind::DrumSynth;
    CHECK(v.start(drum, 0) == 0);
    CHECK(v.unhandled() == 2);
}

TEST_CASE("Voices: stealing takes the oldest releasing voice, else the oldest playing one, without a click",
          "[engine][voices]")
{
    engine::Voices v;
    v.prepare(kRate, kBlock);
    Sample s(48000 * 20, 0.5f);
    v.setSample(0, &s.data);
    std::vector<std::uint64_t> ids;
    for (int i = 0; i < engine::Voices::kSampleVoices; ++i)
    {
        auto e = hit(0);
        if (i == 10)
        {
            e.durationSeconds = 0.001f; // this one lets go at once and has a long release
            e.release = 1.0f;
        }
        ids.push_back(v.start(e, 0));
    }
    Buses b(kBlock);
    v.process(b.get(), kBlock);
    CHECK(v.activeSampleVoices() == 64);
    // Full: the releasing one goes first.
    const auto first = v.start(hit(0), 0);
    CHECK(v.stolen() == 1);
    CHECK_FALSE(v.playing(ids[10]));
    CHECK(v.playing(ids[0]));
    // Then the oldest playing.
    v.start(hit(0), 0);
    CHECK(v.stolen() == 2);
    CHECK_FALSE(v.playing(ids[0]));
    CHECK(v.playing(ids[1]));
    CHECK(v.playing(first));
    CHECK(v.activeSampleVoices() == 64);
    // The stolen voices fade out over 2 ms instead of stopping dead: no jump between samples bigger than a ramp.
    b.clear();
    v.process(b.get(), kBlock);
    float worst = 0.0f;
    for (int i = 1; i < kBlock; ++i)
        worst = std::max(worst, std::abs(b.l[static_cast<std::size_t>(i)] - b.l[static_cast<std::size_t>(i - 1)]));
    CHECK(worst < 0.05f);
    // Synth voices: their own pool of 32.
    for (int i = 0; i < 40; ++i)
    {
        ScheduledEvent e;
        e.kind = VoiceKind::Synth;
        v.start(e, 0);
    }
    CHECK(v.activeSynthVoices() == 32);
    CHECK(v.stolen() == 10);
}

TEST_CASE("Voices: a choke group cuts the open hat when the closed one hits", "[engine][voices]")
{
    engine::Voices v;
    v.prepare(kRate, kBlock);
    Sample s(48000, 1.0f);
    v.setSample(0, &s.data);
    v.setSample(1, &s.data);
    auto open = hit(0, 0.0f); // left
    open.chokeGroup = 1;
    auto closed = hit(1, 1.0f); // right
    closed.chokeGroup = 1;
    const auto openId = v.start(open, 0);
    v.start(closed, 1000);
    Buses b(2048);
    v.process(b.get(), 2048);
    CHECK(b.l[999] > 0.99f);           // the open hat until then
    CHECK(b.l[1000 + 120] < 0.6f);     // fading
    CHECK(std::abs(b.l[1000 + 260]) < 1e-6f); // gone after 5 ms (the closed hat, hard right, leaves ~4e-8)
    CHECK(b.r[1500] > 0.99f);          // the closed one plays
    CHECK_FALSE(v.playing(openId));
    // Another group or source isn't touched.
    v.reset();
    auto other = hit(0, 0.0f);
    other.chokeGroup = 2;
    const auto otherId = v.start(other, 0);
    auto code = closed;
    code.source = engine::SourceId::CodeDeck;
    code.chokeGroup = 2;
    v.start(code, 10);
    b.clear();
    v.process(b.get(), 512);
    CHECK(v.playing(otherId));
}

TEST_CASE("Voices: the synth gate and release, the filters", "[engine][voices]")
{
    engine::Voices v;
    v.prepare(kRate, kBlock);
    ScheduledEvent e;
    e.kind = VoiceKind::Synth;
    e.note = 69.0f; // 440 Hz
    e.durationSeconds = 0.01f;
    e.release = 0.01f;
    const auto id = v.start(e, 0);
    Buses b(2048);
    v.process(b.get(), 2048);
    CHECK_FALSE(v.playing(id)); // 10 ms gate + 10 ms release < 2048 samples
    CHECK(b.l[2000] == 0.0f);
    // A 5 kHz tone through a 200 Hz low-pass: much quieter; through a 200 Hz high-pass: about as loud.
    const auto rms = [&](float cutoff, float hpf)
    {
        v.reset();
        ScheduledEvent t;
        t.kind = VoiceKind::Synth;
        t.note = 69.0f + 12.0f * std::log2(5000.0f / 440.0f);
        t.cutoffHz = cutoff;
        t.hpfHz = hpf;
        v.start(t, 0);
        Buses out(4800);
        v.process(out.get(), 4800);
        double sum = 0.0;
        for (int i = 2400; i < 4800; ++i)
            sum += out.l[static_cast<std::size_t>(i)] * out.l[static_cast<std::size_t>(i)];
        return std::sqrt(sum / 2400.0);
    };
    const double dry = rms(20000.0f, 0.0f);
    CHECK(rms(200.0f, 0.0f) < 0.01 * dry);
    CHECK(rms(20000.0f, 200.0f) > 0.9 * dry);
}

TEST_CASE("Voices: 64 sample voices and 32 synth voices with filters at 48 kHz within budget", "[engine][voices][perf]")
{
    engine::Voices v;
    v.prepare(kRate, kBlock);
    Sample s(48000 * 12, 0.01f);
    v.setSample(0, &s.data);
    for (int i = 0; i < engine::Voices::kSampleVoices; ++i)
    {
        auto e = hit(0, static_cast<float>(i) / 64.0f);
        e.speed = 0.9f + 0.003f * static_cast<float>(i); // interpolating, not on whole samples
        e.cutoffHz = 3000.0f + 100.0f * static_cast<float>(i);
        e.hpfHz = 40.0f;
        e.delaySend = 0.2f;
        e.roomSend = 0.3f;
        v.start(e, i);
    }
    for (int i = 0; i < engine::Voices::kSynthVoices; ++i)
    {
        ScheduledEvent e;
        e.kind = VoiceKind::Synth;
        e.note = 36.0f + static_cast<float>(i);
        e.gain = 0.01f;
        e.cutoffHz = 2000.0f;
        e.roomSend = 0.3f;
        v.start(e, i);
    }
    Buses b(kBlock);
    const int blocks = static_cast<int>(10.0 * kRate / kBlock);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < blocks; ++i)
    {
        b.clear();
        v.process(b.get(), kBlock);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(v.activeSampleVoices() == 64);
    CHECK(v.activeSynthVoices() == 32);
    const double load = seconds / 10.0;
    WARN("96 voices (64 sample + 32 synth, filtered, sends): " << load * 100.0 << " % of real time");
#ifdef NDEBUG
    CHECK(load < 0.15); // the voices' share of a block: with the decks' 35 % that leaves room
#endif
}
