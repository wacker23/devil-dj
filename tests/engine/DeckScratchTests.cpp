// P1-09: jog / scratch from the mouse. Done when: scratching sounds continuous (no zipper noise) at a 60 Hz
// UI event rate. Measured here as the playback rate the deck produces under 60 Hz move events.

#include "engine/AudioEngine.h"
#include "engine/Deck.h"
#include "util/RealtimeGuard.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace devildj::engine;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr double kEventInterval = kRate / 60.0; // 800 samples between UI events

std::unique_ptr<TrackBuffer> sineTrack(double seconds = 20.0, double hz = 1000.0)
{
    auto t = std::make_unique<TrackBuffer>();
    t->sampleRate = kRate;
    const int n = static_cast<int>(seconds * kRate);
    t->audio.setSize(2, n);
    for (int i = 0; i < n; ++i)
    {
        const auto v = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * hz * i / kRate));
        t->audio.setSample(0, i, v);
        t->audio.setSample(1, i, v);
    }
    return t;
}

// Renders sample by sample-block, recording the playhead after every 16 samples (rate resolution).
struct Trace
{
    std::vector<double> rates; // average rate over each 16-sample chunk
    std::vector<float> left;
    double clock = 0.0; // samples rendered so far = the test's audio clock, also used as the UI clock
};

void renderChunk(Deck& d, int samples, Trace& trace)
{
    constexpr int chunk = 16;
    float l[chunk], r[chunk];
    for (int done = 0; done < samples; done += chunk)
    {
        const double before = d.position();
        d.render(l, r, chunk);
        trace.rates.push_back((d.position() - before) / chunk);
        trace.left.insert(trace.left.end(), l, l + chunk);
        trace.clock += chunk;
    }
}

// Drags at a constant hand speed (samples of track per sample of real time) for `seconds`. Like a real mouse:
// a UI event every 16.7 ms reports how far the hand went since the previous one, stamped with the time the UI
// saw it; the audio thread picks it up a little later (optionally jittered, like block boundaries and thread
// scheduling).
void drag(Deck& d, double handRate, double seconds, Trace& trace, double jitterMs = 0.0)
{
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> jitter(0.0, jitterMs);
    const double start = trace.clock;
    double eventTime = start;
    while (eventTime - start < seconds * kRate)
    {
        eventTime += kEventInterval;
        const double pickup = eventTime + jitter(rng) * kRate / 1000.0;
        if (pickup > trace.clock)
            renderChunk(d, static_cast<int>(std::ceil((pickup - trace.clock) / 16.0)) * 16, trace);
        d.scratchMove(handRate * kEventInterval, eventTime / kRate);
    }
}

struct Stats
{
    double mean = 0.0, minV = 1e9, maxV = -1e9, maxStep = 0.0;
};

Stats stats(const std::vector<double>& v, std::size_t from)
{
    Stats s;
    for (auto i = from; i < v.size(); ++i)
    {
        s.mean += v[i];
        s.minV = std::min(s.minV, v[i]);
        s.maxV = std::max(s.maxV, v[i]);
        if (i > from)
            s.maxStep = std::max(s.maxStep, std::abs(v[i] - v[i - 1]));
    }
    s.mean /= static_cast<double>(v.size() - from);
    return s;
}

Deck preparedDeck(const TrackBuffer& t)
{
    Deck d;
    d.prepare(kRate, kBlock);
    d.setTrack(&t);
    return d;
}
} // namespace

TEST_CASE("Scratch: a steady hand at 60 Hz events gives a steady rate (no zipper)", "[engine][scratch]")
{
    const auto t = sineTrack();
    for (double hand : {1.0, -0.7, 2.5})
    {
        auto d = preparedDeck(*t);
        d.seek(10.0 * kRate);
        Trace trace;
        d.scratchBegin(trace.clock / kRate);
        drag(d, hand, 1.0, trace);
        const auto s = stats(trace.rates, trace.rates.size() / 3); // after the first ~0.3 s
        INFO("hand " << hand << ": rate mean " << s.mean << " [" << s.minV << ", " << s.maxV
                     << "], max step per 16 "
                        "samples "
                     << s.maxStep);
        CHECK(std::abs(s.mean - hand) < 0.02 * std::abs(hand));
        CHECK(s.maxV - s.minV < 0.03 * std::abs(hand)); // no 60 Hz wobble
        CHECK(s.maxStep < 0.01 * std::abs(hand) + 1e-6);
    }
}

TEST_CASE("Scratch: jittered pickup (0-5 ms late) stays smooth", "[engine][scratch]")
{
    const auto t = sineTrack();
    auto d = preparedDeck(*t);
    d.seek(5.0 * kRate);
    Trace trace;
    d.scratchBegin(trace.clock / kRate);
    drag(d, 1.0, 1.5, trace, 5.0);
    const auto s = stats(trace.rates, trace.rates.size() / 3);
    INFO("rate [" << s.minV << ", " << s.maxV << "], max step " << s.maxStep);
    CHECK(s.maxV - s.minV < 0.15);
    CHECK(s.maxStep < 0.02);
}

TEST_CASE("Scratch: when the hand stops, the deck settles onto it", "[engine][scratch]")
{
    const auto t = sineTrack();
    auto d = preparedDeck(*t);
    d.seek(4.0 * kRate);
    Trace trace;
    d.scratchBegin(trace.clock / kRate);
    drag(d, 1.0, 0.5, trace);
    const double handEnd = 4.0 * kRate + 0.5 * kRate;      // where the hand stopped (approximately)
    renderChunk(d, static_cast<int>(0.15 * kRate), trace); // no more moves
    CHECK(std::abs(d.currentRate()) < 0.01);
    CHECK(std::abs(d.position() - handEnd) < 0.01 * kRate);
}

TEST_CASE("Scratch: releasing a playing deck eases back to the normal rate in 50 ms", "[engine][scratch]")
{
    const auto t = sineTrack();
    auto d = preparedDeck(*t);
    d.setTempoPercent(5.0);
    d.seek(5.0 * kRate); // room to scratch backwards without hitting the start
    d.play();
    Trace trace;
    renderChunk(d, kBlock * 4, trace);
    d.scratchBegin(trace.clock / kRate);
    drag(d, -1.0, 0.3, trace); // scratch backwards
    CHECK(d.currentRate() < 0.0);
    d.scratchEnd();
    renderChunk(d, static_cast<int>(0.025 * kRate), trace); // halfway through the ramp
    const double mid = d.currentRate();
    CHECK(mid > -1.0);
    CHECK(mid < 1.05);
    renderChunk(d, static_cast<int>(0.04 * kRate), trace);
    CHECK_FALSE(d.isScratching());
    CHECK(std::abs(d.currentRate() - 1.05) < 1e-9);
    CHECK(d.isPlaying());
    // The ramp is continuous: no big jumps in rate between 16-sample chunks.
    CHECK(stats(trace.rates, 0).maxStep < 0.2);
}

TEST_CASE("Scrub: dragging a paused deck is audible, and it stays paused on release", "[engine][scratch]")
{
    const auto t = sineTrack();
    auto d = preparedDeck(*t);
    d.seek(2.0 * kRate);
    Trace trace;
    d.scratchBegin(trace.clock / kRate);
    drag(d, 0.5, 0.4, trace);
    double energy = 0.0;
    for (float v : trace.left)
        energy += static_cast<double>(v) * v;
    CHECK(energy > 100.0); // we heard the scrub
    const double where = d.position();
    d.scratchEnd();
    CHECK_FALSE(d.isPlaying());
    Trace after;
    renderChunk(d, 512, after);
    CHECK(d.isSilent());
    CHECK(d.position() == where);
}

TEST_CASE("Scratch: the hand can't push past the start or the end", "[engine][scratch]")
{
    const auto t = sineTrack(2.0);
    auto d = preparedDeck(*t);
    d.seek(0.1 * kRate);
    Trace trace;
    d.scratchBegin(trace.clock / kRate);
    drag(d, -3.0, 0.3, trace);
    CHECK(d.position() >= 0.0);
    CHECK(d.position() < 0.01 * kRate);
    drag(d, 8.0, 0.5, trace);
    CHECK(d.position() <= 2.0 * kRate);
    for (double r : trace.rates)
        REQUIRE(std::isfinite(r));
}

TEST_CASE("Nudge: the wheel shifts a playing deck smoothly, a paused one silently", "[engine][scratch]")
{
    const auto t = sineTrack();
    auto playing = preparedDeck(*t);
    auto reference = preparedDeck(*t);
    playing.play();
    reference.play();
    Trace a, b;
    renderChunk(playing, kBlock, a);
    renderChunk(reference, kBlock, b);
    playing.nudge(480.0); // +10 ms
    renderChunk(playing, static_cast<int>(0.2 * kRate), a);
    renderChunk(reference, static_cast<int>(0.2 * kRate), b);
    CHECK(std::abs((playing.position() - reference.position()) - 480.0) < 1e-6);
    CHECK(stats(a.rates, 0).maxV <= 1.0 + Deck::kMaxNudgeRateOffset + 1e-9); // never faster than +5 %

    auto paused = preparedDeck(*t);
    paused.seek(1000.0);
    paused.nudge(-240.0);
    CHECK(paused.position() == 760.0);
    CHECK(paused.isSilent());
}

TEST_CASE("Scratch: starting and stopping don't click", "[engine][scratch]")
{
    const auto t = sineTrack(20.0, 200.0);
    auto d = preparedDeck(*t);
    d.play();
    Trace trace;
    renderChunk(d, 4800, trace);
    d.scratchBegin(trace.clock / kRate); // hand down: the record stops in ~5 ms
    drag(d, 0.0, 0.1, trace);
    drag(d, 1.0, 0.3, trace);
    d.scratchEnd();
    renderChunk(d, 4800, trace);
    // Bound: the sine's slope at the highest rate reached, plus margin for the smoother.
    const double slope = 2.0 * std::numbers::pi * 200.0 * 0.5 / kRate;
    float worst = 0.0f;
    for (std::size_t i = 1; i < trace.left.size(); ++i)
        worst = std::max(worst, std::abs(trace.left[i] - trace.left[i - 1]));
    INFO("max step " << worst << " vs sine slope " << slope);
    CHECK(worst <= 1.5 * slope);
}

TEST_CASE("Scratch bypasses key lock and returns to it afterwards", "[engine][scratch]")
{
    const auto t = sineTrack(20.0, 440.0);
    auto d = preparedDeck(*t);
    d.setKeyLock(true);
    d.setTempoPercent(8.0);
    d.play();
    Trace trace;
    renderChunk(d, static_cast<int>(0.5 * kRate), trace);
    d.scratchBegin(trace.clock / kRate);
    drag(d, 0.5, 0.3, trace);
    d.scratchEnd();
    renderChunk(d, static_cast<int>(0.3 * kRate), trace);
    CHECK(d.keyLock());
    CHECK_FALSE(d.isScratching());
    CHECK(std::abs(d.currentRate() - 1.08) < 1e-9);
}

TEST_CASE("Engine: jog commands reach the deck; no allocation while scratching", "[engine][scratch]")
{
    const auto t = sineTrack();
    AudioEngine e;
    e.prepare(kRate, kBlock);
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[] = {l.data(), r.data()};
    e.send(LoadTrack{0, t.get(), 1});
    e.send(Play{0});
    e.process(outs, 2, kBlock);

    const auto before = devildj::util::realtimeViolationCount();
    e.send(ScratchBegin{0});
    for (int i = 0; i < 60; ++i)
    {
        if (i % 3 == 0)
            e.send(ScratchMove{0, -800.0}); // backwards, ~60 Hz at 256-sample blocks
        e.process(outs, 2, kBlock);
    }
    CHECK(e.readState().decks[0].scratching);
    CHECK(e.readState().decks[0].rate < 0.0);
    e.send(ScratchEnd{0});
    e.send(Nudge{0, 240.0});
    for (int i = 0; i < 20; ++i)
        e.process(outs, 2, kBlock);
    CHECK_FALSE(e.readState().decks[0].scratching);
    if (devildj::util::kRealtimeGuardEnabled)
        CHECK(devildj::util::realtimeViolationCount() == before);
}

TEST_CASE("Scratch: moves that only land on 256-sample block boundaries stay smooth", "[engine][scratch]")
{
    // Real case: the UI sends a move every ~16.7 ms, but the audio thread only picks commands up at the start of
    // a block, so the deck sees them 768 or 1024 samples apart while each carries 800 samples of hand travel.
    const auto t = sineTrack();
    auto d = preparedDeck(*t);
    d.seek(5.0 * kRate);
    Trace trace;
    d.scratchBegin(trace.clock / kRate);
    double nextEvent = kEventInterval, now = 0.0, pending = 0.0, lastEvent = 0.0;
    while (now < 1.5 * kRate)
    {
        while (nextEvent <= now)
        {
            pending += kEventInterval; // hand speed 1.0
            lastEvent = nextEvent;
            nextEvent += kEventInterval;
        }
        if (pending != 0.0)
        {
            d.scratchMove(pending, lastEvent / kRate);
            pending = 0.0;
        }
        renderChunk(d, kBlock, trace);
        now += kBlock;
    }
    const auto s = stats(trace.rates, trace.rates.size() / 3);
    INFO("rate mean " << s.mean << " [" << s.minV << ", " << s.maxV << "], max step " << s.maxStep);
    CHECK(std::abs(s.mean - 1.0) < 0.02);
    CHECK(s.maxV - s.minV < 0.1);
    CHECK(s.maxStep < 0.01);
}
