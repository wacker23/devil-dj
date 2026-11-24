// P6-05: Drum Swap - loops in place of a deck's drums stem, on the beatgrid, switched on the bar without a click.

#include "engine/AudioEngine.h"
#include "engine/StemLoader.h"
#include "util/RealtimeGuard.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

using namespace devildj;
using namespace devildj::engine;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr double kBpm = 120.0;
constexpr double kBeat = kRate * 60.0 / kBpm; // 24000 samples
constexpr double kBar = 4.0 * kBeat;          // 96000
const int kFade = static_cast<int>(std::lround(Deck::kSwapFadeSeconds * kRate)); // 192

// A track whose stems are: drums `drums`, the rest silent (or a quiet sine for `music`).
struct Track
{
    std::unique_ptr<TrackBuffer> buffer = std::make_unique<TrackBuffer>();
    std::unique_ptr<StemBuffer> stems = std::make_unique<StemBuffer>();
    Track(int n, float drums, bool music = false)
    {
        buffer->sampleRate = kRate;
        buffer->audio.setSize(2, n);
        stems->length = n;
        stems->sampleRate = kRate;
        stems->track = buffer.get();
        for (int s = 0; s < 4; ++s)
            for (int ch = 0; ch < 2; ++ch)
                stems->samples[static_cast<std::size_t>(s * 2 + ch)].assign(static_cast<std::size_t>(n), 0);
        for (int i = 0; i < n; ++i)
        {
            const float other = music ? static_cast<float>(0.1 * std::sin(2.0 * std::numbers::pi * 220.0 * i / kRate)) : 0.0f;
            const auto d = toInt16(drums), o = toInt16(other);
            for (int ch = 0; ch < 2; ++ch)
            {
                stems->samples[static_cast<std::size_t>(ch)][static_cast<std::size_t>(i)] = d;
                stems->samples[static_cast<std::size_t>(4 + ch)][static_cast<std::size_t>(i)] = o;
                buffer->audio.setSample(ch, i, (d + o) / 32768.0f);
            }
        }
    }
};

// A loop of `beats` beats at the deck's tempo: constant `value`, or (ramp) its own sample index / length.
std::unique_ptr<TrackBuffer> loop(double beats, float value, bool ramp = false)
{
    auto t = std::make_unique<TrackBuffer>();
    t->sampleRate = kRate;
    const auto n = static_cast<int>(std::lround(beats * kBeat));
    t->audio.setSize(2, n);
    for (int i = 0; i < n; ++i)
        for (int ch = 0; ch < 2; ++ch)
            t->audio.setSample(ch, i, ramp ? static_cast<float>(i) / static_cast<float>(n) * value : value);
    return t;
}

// The engine with the track on deck A, its stems and a 120 BPM grid from sample 0; output and positions logged.
struct Rig
{
    AudioEngine e;
    Track track;
    std::vector<float> out;          // left output
    std::vector<double> position;    // the deck's position heard at each output sample
    float centre = static_cast<float>(crossfaderGainA(0.5, CrossfaderCurve::Smooth));
    explicit Rig(float drums = 0.2f, bool music = false, int seconds = 12)
        : track(static_cast<int>(seconds * kRate), drums, music)
    {
        e.prepare(kRate, kBlock);
        e.send(LoadTrack{0, track.buffer.get(), 1});
        e.send(LoadStems{0, track.buffer.get(), track.stems.get(), 2});
        e.send(SetBeatgrid{0, util::Beatgrid::constant(0.0, kBpm, kRate)});
        blocks(1);
    }
    void blocks(int n)
    {
        std::vector<float> l(kBlock), r(kBlock);
        float* outs[] = {l.data(), r.data()};
        for (int b = 0; b < n; ++b)
        {
            const double start = e.readState().decks[0].positionSamples;
            const double rate = e.readState().decks[0].playing ? e.readState().decks[0].rate : 0.0;
            e.process(outs, 2, kBlock);
            const double latency = e.readState().masterLatencySamples; // the limiter's look-ahead
            for (int i = 0; i < kBlock; ++i)
            {
                out.push_back(l[static_cast<std::size_t>(i)]);
                position.push_back(start + rate * (i - latency));
            }
        }
    }
    const DeckState& deck() { return e.readState().decks[0]; }
};
} // namespace

TEST_CASE("Drum Swap: a loop replaces the drums stem, in step with the grid", "[engine][drumswap]")
{
    Rig r(0.2f);
    auto ramp = loop(4.0, 0.5f, true);
    r.e.send(SetSwapLoop{0, 0, ramp.get(), 10, 4.0f});
    r.e.send(SetBeatgrid{0, util::Beatgrid::constant(0.0, kBpm, kRate)});
    r.e.send(DrumSwap{0, 0, DrumSwapOp::Single}); // paused: at once, from the bar at the playhead (sample 0)
    r.blocks(1);
    CHECK(r.deck().swapActive == 1);
    r.e.send(Play{0});
    r.blocks(4 * 60); // ~1.3 s
    // Past the fade: the output is the loop alone, at the deck's position modulo its 4 beats.
    double worst = 0.0;
    for (std::size_t i = r.out.size() - 20000; i < r.out.size(); ++i)
    {
        const double p = std::fmod(r.position[i], 4.0 * kBeat);
        const double expect = r.centre * 0.5 * p / (4.0 * kBeat);
        worst = std::max(worst, std::abs(r.out[i] - expect));
    }
    INFO("worst " << worst);
    CHECK(worst < 1e-4);
}

TEST_CASE("Drum Swap: playing, the swap is on the next bar - the 4 ms fade ends on it", "[engine][drumswap]")
{
    Rig r(0.2f);
    auto flat = loop(4.0, -0.3f);
    r.e.send(SetSwapLoop{0, 0, flat.get(), 10, 4.0f});
    r.e.send(Play{0});
    r.blocks(20); // ~5000 samples into bar 1
    r.e.send(DrumSwap{0, 0, DrumSwapOp::Single});
    r.blocks(1);
    CHECK(r.deck().swapQueued == 1);
    CHECK(r.deck().swapActive == 0);
    r.blocks(400); // past sample 96000
    CHECK(r.deck().swapActive == 1);
    CHECK(r.deck().swapQueued == -1);
    // Where the output leaves the drums and where it is all loop, in deck positions.
    const float drums = r.centre * 0.2f, swapped = r.centre * -0.3f;
    double first = -1.0, full = -1.0;
    for (std::size_t i = 0; i < r.out.size(); ++i)
    {
        if (first < 0.0 && r.position[i] > 1000.0 && std::abs(r.out[i] - drums) > 1e-5f)
            first = r.position[i];
        if (full < 0.0 && std::abs(r.out[i] - swapped) < 1e-5f)
            full = r.position[i];
    }
    INFO("fade from " << first << " to " << full << ", the bar at " << kBar);
    CHECK(std::abs(full - kBar) <= 1.0);
    CHECK(std::abs(first - (kBar - kFade)) <= 1.0);
}

TEST_CASE("Drum Swap: no click - the fade is smooth on music", "[engine][drumswap]")
{
    Rig r(0.0f, true);
    auto beat = loop(1.0, 0.0f);
    for (int i = 0; i < beat->numSamples(); ++i) // a steady tone with a constant offset in the loop: a step if cut
        for (int ch = 0; ch < 2; ++ch)
            beat->audio.setSample(ch, i, 0.25f + static_cast<float>(0.05 * std::sin(2.0 * std::numbers::pi * 100.0 * i / kRate)));
    r.e.send(SetSwapLoop{0, 3, beat.get(), 10, 1.0f});
    r.e.send(Play{0});
    r.blocks(20);
    r.e.send(DrumSwap{0, 3, DrumSwapOp::Single});
    r.blocks(400);
    REQUIRE(r.deck().swapActive == (1 << 3));
    double step = 0.0;
    for (std::size_t i = 1; i < r.out.size(); ++i)
        step = std::max(step, static_cast<double>(std::abs(r.out[i] - r.out[i - 1])));
    INFO("largest sample-to-sample step " << step);
    CHECK(step < 0.01); // a cut would step 0.25 x the crossfader gain; the 1-beat loop wraps seamlessly
    // And back to the drums on the next bar the same way.
    r.e.send(DrumSwap{0, 3, DrumSwapOp::Single});
    r.blocks(400);
    CHECK(r.deck().swapActive == 0);
    for (std::size_t i = 1; i < r.out.size(); ++i)
        step = std::max(step, static_cast<double>(std::abs(r.out[i] - r.out[i - 1])));
    CHECK(step < 0.01);
}

TEST_CASE("Drum Swap: single switches between slots, layer adds them up, off brings the drums back", "[engine][drumswap]")
{
    Rig r(0.2f);
    auto a = loop(4.0, 0.1f), b = loop(4.0, 0.2f), c = loop(8.0, 0.4f);
    r.e.send(SetSwapLoop{0, 0, a.get(), 10, 4.0f});
    r.e.send(SetSwapLoop{0, 1, b.get(), 11, 4.0f});
    r.e.send(SetSwapLoop{0, 2, c.get(), 12, 8.0f});
    r.blocks(1);
    CHECK(r.deck().swapLoaded == 0x07);
    // Paused: each at once.
    r.e.send(DrumSwap{0, 0, DrumSwapOp::Single});
    r.e.send(DrumSwap{0, 1, DrumSwapOp::Single});
    r.blocks(1);
    CHECK(r.deck().swapActive == 0x02);
    r.e.send(DrumSwap{0, 1, DrumSwapOp::Single}); // the one playing: the drums back
    r.blocks(1);
    CHECK(r.deck().swapActive == 0);
    r.e.send(DrumSwap{0, 0, DrumSwapOp::Layer});
    r.e.send(DrumSwap{0, 2, DrumSwapOp::Layer});
    r.blocks(1);
    CHECK(r.deck().swapActive == 0x05);
    r.e.send(Play{0});
    r.blocks(10);
    CHECK(std::abs(r.out.back() - r.centre * 0.5f) < 1e-5f); // 0.1 + 0.4 in place of the drums
    // Playing: several presses before the bar build one change.
    r.e.send(DrumSwap{0, 0, DrumSwapOp::Layer});
    r.e.send(DrumSwap{0, 1, DrumSwapOp::Layer});
    r.blocks(1);
    CHECK(r.deck().swapQueued == 0x06);
    r.e.send(DrumSwap{0, 7, DrumSwapOp::Layer}); // an empty slot: nothing
    r.blocks(1);
    CHECK(r.deck().swapQueued == 0x06);
    r.e.send(DrumSwap{0, 0, DrumSwapOp::Off});
    r.blocks(1);
    CHECK(r.deck().swapQueued == 0);
    r.blocks(400);
    CHECK(r.deck().swapActive == 0);
    CHECK(std::abs(r.out.back() - r.centre * 0.2f) < 1e-5f);
}

TEST_CASE("Drum Swap: without stems nothing happens; loops are handed back when replaced; a new track keeps them",
          "[engine][drumswap]")
{
    AudioEngine e;
    e.prepare(kRate, kBlock);
    Track t(48000, 0.2f);
    auto a = loop(4.0, 0.1f), b = loop(4.0, 0.2f);
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[] = {l.data(), r.data()};
    const auto released = [&]
    {
        std::vector<BufferId> ids;
        BufferId id = 0;
        while (e.popReleasedBuffer(id))
            ids.push_back(id);
        return ids;
    };
    e.send(LoadTrack{0, t.buffer.get(), 1});
    e.send(SetSwapLoop{0, 0, a.get(), 10, 4.0f});
    e.send(DrumSwap{0, 0, DrumSwapOp::Single});
    e.process(outs, 2, kBlock);
    CHECK(e.readState().decks[0].swapActive == 0); // no stems
    e.send(SetSwapLoop{0, 0, b.get(), 11, 4.0f});
    e.process(outs, 2, kBlock);
    CHECK(released() == std::vector<BufferId>{10});
    e.send(SetSwapLoop{0, 0, nullptr, 0, 0.0f});
    e.process(outs, 2, kBlock);
    CHECK(released() == std::vector<BufferId>{11});
    CHECK(e.readState().decks[0].swapLoaded == 0);

    e.send(SetSwapLoop{0, 5, a.get(), 12, 4.0f});
    e.send(LoadStems{0, t.buffer.get(), t.stems.get(), 2});
    e.send(DrumSwap{0, 5, DrumSwapOp::Single});
    e.process(outs, 2, kBlock);
    CHECK(e.readState().decks[0].swapActive == (1 << 5));
    Track next(48000, 0.1f);
    e.send(LoadTrack{0, next.buffer.get(), 3});
    e.process(outs, 2, kBlock);
    CHECK(e.readState().decks[0].swapLoaded == (1 << 5)); // the bank stays with the deck
    CHECK(e.readState().decks[0].swapActive == 0);         // the new track plays its own drums
}

TEST_CASE("Drum Swap: with key lock the swap is still heard on the bar", "[engine][drumswap][keylock]")
{
    Rig r(0.2f);
    auto flat = loop(4.0, -0.3f);
    r.e.send(SetSwapLoop{0, 0, flat.get(), 10, 4.0f});
    r.e.send(SetKeyLock{0, true});
    r.e.send(Play{0});
    r.blocks(40);
    r.e.send(DrumSwap{0, 0, DrumSwapOp::Single});
    r.blocks(420);
    REQUIRE(r.deck().swapActive == 1);
    // The stretcher smears the step; its middle should be the fade's middle, 2 ms before the bar.
    const float mid = r.centre * (0.2f - 0.3f) / 2.0f;
    double crossing = -1.0;
    for (std::size_t i = 1; i < r.out.size() && crossing < 0.0; ++i)
        if (r.position[i] > 2.0 * kBeat && r.out[i - 1] > mid && r.out[i] <= mid)
            crossing = r.position[i];
    INFO("heard at " << crossing << ", the bar at " << kBar);
    REQUIRE(crossing > 0.0);
    CHECK(std::abs(crossing - (kBar - kFade / 2.0)) < 0.005 * kRate); // within 5 ms
}

TEST_CASE("Drum Swap: swapping doesn't allocate on the audio thread", "[engine][drumswap]")
{
    if (!util::kRealtimeGuardEnabled)
        SKIP("RealtimeGuard is only active in Debug builds");
    Rig r(0.2f, true, 4);
    auto a = loop(4.0, 0.1f), b = loop(1.0, 0.2f);
    const auto before = util::realtimeViolationCount();
    r.e.send(SetSwapLoop{0, 0, a.get(), 10, 4.0f});
    r.e.send(SetSwapLoop{0, 1, b.get(), 11, 1.0f});
    r.e.send(Play{0});
    r.blocks(10);
    r.e.send(DrumSwap{0, 0, DrumSwapOp::Layer});
    r.e.send(DrumSwap{0, 1, DrumSwapOp::Layer});
    r.blocks(10);
    r.e.send(SetKeyLock{0, true});
    r.blocks(10);
    r.e.send(DrumSwap{0, 0, DrumSwapOp::Off});
    r.blocks(10);
    CHECK(util::realtimeViolationCount() == before);
}
