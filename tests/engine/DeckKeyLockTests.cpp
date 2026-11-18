// P1-08: key lock. Done when: 440 Hz at +10 % tempo stays 440 Hz +-1 Hz with key lock; CPU for 2 decks with
// key lock < 15 % of the 256-sample budget at 48 kHz (measured and logged).

#include "engine/AudioEngine.h"
#include "engine/Deck.h"
#include "engine/TrackDecoder.h"
#include "util/RealtimeGuard.h"

#include "TestData.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <vector>

using namespace devildj::engine;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

std::unique_ptr<TrackBuffer> sineTrack(double hz, double seconds, float amp = 0.5f)
{
    auto t = std::make_unique<TrackBuffer>();
    t->sampleRate = kRate;
    const int n = static_cast<int>(seconds * kRate);
    t->audio.setSize(2, n);
    for (int i = 0; i < n; ++i)
    {
        const auto v = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * i / kRate));
        t->audio.setSample(0, i, v);
        t->audio.setSample(1, i, v);
    }
    return t;
}

struct Recorded
{
    std::vector<float> left;
    std::vector<double> playheadAtBlockStart; // per block
};

Recorded render(Deck& d, int samples, int block = kBlock)
{
    Recorded out;
    std::vector<float> l(static_cast<std::size_t>(block)), r(static_cast<std::size_t>(block));
    for (int done = 0; done < samples; done += block)
    {
        out.playheadAtBlockStart.push_back(d.position());
        d.render(l.data(), r.data(), block);
        out.left.insert(out.left.end(), l.begin(), l.end());
    }
    return out;
}

// Frequency from interpolated rising zero crossings between two times (seconds of output).
double frequency(const std::vector<float>& x, double fromSec, double toSec)
{
    const auto from = static_cast<std::size_t>(fromSec * kRate), to = static_cast<std::size_t>(toSec * kRate);
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (auto i = from + 1; i < to; ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double t = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0.0)
                first = t;
            last = t;
            ++crossings;
        }
    return crossings > 1 ? (crossings - 1) * kRate / (last - first) : 0.0;
}

double rms(const std::vector<float>& x, double fromSec, double toSec)
{
    const auto from = static_cast<std::size_t>(fromSec * kRate), to = static_cast<std::size_t>(toSec * kRate);
    double s = 0.0;
    for (auto i = from; i < to; ++i)
        s += static_cast<double>(x[i]) * x[i];
    return std::sqrt(s / static_cast<double>(to - from));
}

Deck keyLockedDeck(const TrackBuffer& t)
{
    Deck d;
    d.prepare(kRate, kBlock);
    d.setTrack(&t);
    d.setKeyLock(true);
    return d;
}
} // namespace

TEST_CASE("Key lock: 440 Hz at +10 % tempo stays 440 Hz (+-1 Hz)", "[engine][keylock]")
{
    const auto t = sineTrack(440.0, 12.0);
    auto d = keyLockedDeck(*t);
    d.setTempoPercent(10.0);
    d.play();
    const auto out = render(d, static_cast<int>(6.0 * kRate));
    const double hz = frequency(out.left, 1.0, 5.0);
    INFO("measured " << hz << " Hz, level " << rms(out.left, 1.0, 5.0));
    CHECK(std::abs(hz - 440.0) < 1.0);
    CHECK(std::abs(rms(out.left, 1.0, 5.0) - 0.5 / std::sqrt(2.0)) < 0.05); // level kept
    CHECK(std::abs(d.position() - 6.0 * kRate * 1.1) < 2.0);                // still plays 10 % faster
}

TEST_CASE("Without key lock the same setting raises the pitch to 484 Hz", "[engine][keylock]")
{
    const auto t = sineTrack(440.0, 12.0);
    Deck d;
    d.prepare(kRate, kBlock);
    d.setTrack(t.get());
    d.setTempoPercent(10.0);
    d.play();
    const auto out = render(d, static_cast<int>(3.0 * kRate));
    CHECK(std::abs(frequency(out.left, 0.5, 2.5) - 484.0) < 1.0);
}

TEST_CASE("Key lock holds the pitch across the tempo range", "[engine][keylock]")
{
    const auto t = sineTrack(440.0, 20.0);
    for (double percent : {-16.0, -6.0, 6.0, 16.0})
    {
        auto d = keyLockedDeck(*t);
        d.setTempoRange(16.0);
        d.setTempoPercent(percent);
        d.play();
        const auto out = render(d, static_cast<int>(4.0 * kRate));
        const double hz = frequency(out.left, 1.0, 3.5);
        INFO(percent << " % -> " << hz << " Hz");
        CHECK(std::abs(hz - 440.0) < 1.0);
    }
}

TEST_CASE("Key Shift transposes by semitones (with key lock)", "[engine][keylock]")
{
    const auto t = sineTrack(440.0, 12.0);
    for (int semis : {1, -2})
    {
        auto d = keyLockedDeck(*t);
        d.setKeyShift(semis);
        d.play();
        const auto out = render(d, static_cast<int>(4.0 * kRate));
        const double expected = 440.0 * std::pow(2.0, semis / 12.0);
        const double hz = frequency(out.left, 1.0, 3.5);
        INFO(semis << " semitones: expected " << expected << ", measured " << hz);
        CHECK(std::abs(hz - expected) < 1.0);
        CHECK(d.keyShift() == semis);
    }
}

TEST_CASE("Key lock: the reported position is what you hear (latency compensated)", "[engine][keylock]")
{
    const auto file = devildj::test::generatedFile("clicks/click_120bpm.wav");
    if (!file.existsAsFile())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");

    DecodeOptions options;
    options.targetSampleRate = kRate;
    auto decoded = decodeTrack(file, options);
    REQUIRE(decoded.isOk());
    const auto track = std::move(decoded).value();
    const double firstClick = 0.5 * kRate; // 0.5 s, from the generator manifest

    for (double percent : {0.0, 10.0})
    {
        auto d = keyLockedDeck(*track);
        d.setTempoPercent(percent);
        d.seek(firstClick - 0.25 * kRate); // start 250 ms before the click
        d.play();
        const auto out = render(d, static_cast<int>(1.0 * kRate));

        const auto peak = *std::max_element(out.left.begin(), out.left.end());
        std::size_t onset = 0;
        while (onset < out.left.size() && std::abs(out.left[onset]) < 0.25f * peak)
            ++onset;
        REQUIRE(onset < out.left.size());

        // Where the deck said it was when that sample played.
        const auto block = onset / kBlock;
        const double reported = out.playheadAtBlockStart[block] + static_cast<double>(onset % kBlock) * d.rate();
        const double errorMs = (reported - firstClick) / kRate * 1000.0;
        INFO(percent << " %: click heard when the deck reported " << reported << " (" << errorMs << " ms off)");
        CHECK(std::abs(errorMs) < 5.0);
    }
}

TEST_CASE("Key lock: switching on and off while playing doesn't click", "[engine][keylock]")
{
    const auto t = sineTrack(1000.0, 10.0);
    Deck d;
    d.prepare(kRate, kBlock);
    d.setTrack(t.get());
    d.play();
    std::vector<float> all;
    auto run = [&](int samples)
    {
        const auto o = render(d, samples);
        all.insert(all.end(), o.left.begin(), o.left.end());
    };
    run(static_cast<int>(0.5 * kRate));
    d.setKeyLock(true);
    run(static_cast<int>(0.5 * kRate));
    d.setKeyLock(false);
    run(static_cast<int>(0.5 * kRate));

    const auto slope = static_cast<float>(2.0 * std::numbers::pi * 1000.0 * 0.5 / kRate);
    float worst = 0.0f;
    for (std::size_t i = 1; i < all.size(); ++i)
        worst = std::max(worst, std::abs(all[i] - all[i - 1]));
    INFO("max step " << worst << " vs sine slope " << slope);
    CHECK(worst <= 1.5f * slope);
}

TEST_CASE("Key lock: seeking and cueing keep playing the right material", "[engine][keylock]")
{
    // Left channel: 440 Hz for the first 5 s, then 880 Hz. After a jump into the second half the key-locked
    // output must be 880 Hz.
    auto t = sineTrack(440.0, 10.0);
    for (int i = static_cast<int>(5.0 * kRate); i < t->numSamples(); ++i)
    {
        const auto v = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 880.0 * i / kRate));
        t->audio.setSample(0, i, v);
        t->audio.setSample(1, i, v);
    }
    auto d = keyLockedDeck(*t);
    d.setTempoPercent(5.0);
    d.play();
    render(d, static_cast<int>(1.0 * kRate));
    d.seek(6.0 * kRate);
    const auto out = render(d, static_cast<int>(2.0 * kRate));
    CHECK(std::abs(frequency(out.left, 0.3, 1.5) - 880.0) < 2.0);
}

TEST_CASE("Key lock doesn't allocate on the audio thread", "[engine][keylock]")
{
    if (!devildj::util::kRealtimeGuardEnabled)
        SKIP("RealtimeGuard is only active in Debug builds");

    const auto t = sineTrack(440.0, 20.0);
    AudioEngine e;
    e.prepare(kRate, kBlock);
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[] = {l.data(), r.data()};
    e.send(LoadTrack{0, t.get(), 1});
    e.send(SetKeyLock{0, true});
    e.send(SetTempo{0, 8.0});
    e.process(outs, 2, kBlock); // warm up

    const auto before = devildj::util::realtimeViolationCount();
    e.send(Play{0});
    for (int i = 0; i < 400; ++i)
    {
        if (i == 100)
            e.send(Seek{0, 300000.0});
        if (i == 150)
            e.send(SetKeyShift{0, 2});
        if (i == 200)
            e.send(PitchBend{0, 4.0});
        if (i == 250)
            e.send(SetKeyLock{0, false});
        if (i == 300)
            e.send(SetKeyLock{0, true});
        e.process(outs, 2, kBlock);
    }
    CHECK(devildj::util::realtimeViolationCount() == before);
}

namespace
{
// Two decks with key lock at +10 %, 256-sample blocks at 48 kHz, like the plan's measurement.
struct CpuResult
{
    double meanPercent = 0.0, p99Percent = 0.0, maxPercent = 0.0;
};

CpuResult measureTwoKeyLockedDecks(double seconds)
{
    const auto a = sineTrack(440.0, seconds + 2.0), b = sineTrack(330.0, seconds + 2.0);
    AudioEngine e;
    e.prepare(kRate, kBlock);
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[] = {l.data(), r.data()};
    for (DeckId deck : {DeckId{0}, DeckId{1}})
    {
        e.send(LoadTrack{deck, deck == 0 ? a.get() : b.get(), static_cast<BufferId>(deck + 1)});
        e.send(SetKeyLock{deck, true});
        e.send(SetTempo{deck, 10.0});
        e.send(Play{deck});
    }
    for (int i = 0; i < 50; ++i) // warm up
        e.process(outs, 2, kBlock);

    const double budget = kBlock / kRate;
    std::vector<double> loads;
    const int blocks = static_cast<int>(seconds * kRate / kBlock);
    for (int i = 0; i < blocks; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        e.process(outs, 2, kBlock);
        loads.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() / budget);
    }
    std::sort(loads.begin(), loads.end());
    CpuResult result;
    for (double v : loads)
        result.meanPercent += v;
    result.meanPercent = 100.0 * result.meanPercent / static_cast<double>(loads.size());
    result.p99Percent = 100.0 * loads[loads.size() * 99 / 100];
    result.maxPercent = 100.0 * loads.back();
    return result;
}
} // namespace

TEST_CASE("Key lock CPU stays well inside the audio budget (sanity)", "[engine][keylock]")
{
#if !defined(NDEBUG)
    SKIP("CPU numbers only mean something in an optimised (Release) build");
#else
    const auto r = measureTwoKeyLockedDecks(3.0);
    WARN("2 decks key lock +10 %, 256 @ 48 kHz: mean " << r.meanPercent << " %, p99 " << r.p99Percent << " %, max "
                                                       << r.maxPercent << " % of the block budget");
    // Loose so slower CI machines pass; the real target is checked by the [.perf] test below.
    CHECK(r.meanPercent < 60.0);
#endif
}

// Run on the target laptop (Release): DevilDJTests.exe "[.perf]"
TEST_CASE("Key lock CPU: 2 decks < 15 % of the 256-sample budget", "[.perf][keylock]")
{
    const auto r = measureTwoKeyLockedDecks(30.0);
    WARN("2 decks key lock +10 %, 256 @ 48 kHz over 30 s: mean " << r.meanPercent << " %, p99 " << r.p99Percent
                                                                 << " %, max " << r.maxPercent << " %");
    CHECK(r.meanPercent < 15.0);
}
