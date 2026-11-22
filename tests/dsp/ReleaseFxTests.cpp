// P3-11: release FX. Done when: manual test; works with Pad FX (P3-12). These check the engine side: idle is
// bit-exact; held = the character on the music; let go = the music cut and the effect ending it over the beat
// length; then the deck pauses and the channel is back to normal.

#include "dsp/fx/ReleaseFx.h"
#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

using namespace devildj;
using dsp::fx::ReleaseFx;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr double kBeat = 60.0 / 128.0 * kRate; // 22500

struct Runner
{
    ReleaseFx fx;
    int at = 0;
    dsp::fx::BeatContext beat{128.0, 0.0, 128.0 / (60.0 * kRate)};
    Runner() { fx.prepare(kRate, kBlock); }
    std::vector<float> block(double hz = 220.0, double level = 0.5)
    {
        std::vector<float> l(kBlock), r(kBlock);
        for (int i = 0; i < kBlock; ++i)
            l[static_cast<std::size_t>(i)] = r[static_cast<std::size_t>(i)] =
                static_cast<float>(level * std::sin(2.0 * 3.141592653589793 * hz * (at + i) / kRate));
        fx.process(l.data(), r.data(), kBlock, beat);
        at += kBlock;
        beat.beatAtBlockStart += kBlock * beat.beatsPerSample;
        return l;
    }
    std::vector<float> blocks(int n, double hz = 220.0)
    {
        std::vector<float> all;
        for (int b = 0; b < n; ++b)
        {
            const auto o = block(hz);
            all.insert(all.end(), o.begin(), o.end());
        }
        return all;
    }
};

double rms(const std::vector<float>& v, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    to = std::min(to, v.size());
    for (std::size_t i = from; i < to; ++i)
        sum += static_cast<double>(v[i]) * v[i];
    return to > from ? std::sqrt(sum / static_cast<double>(to - from)) : 0.0;
}

// Zero crossings per sample over a span: how fast the (sine) audio plays.
double crossings(const std::vector<float>& v, std::size_t from, std::size_t to)
{
    int n = 0;
    for (std::size_t i = from + 1; i < to && i < v.size(); ++i)
        n += (v[i - 1] < 0.0f) != (v[i] < 0.0f) ? 1 : 0;
    return static_cast<double>(n) / static_cast<double>(to - from);
}
} // namespace

TEST_CASE("Release FX: idle is bit-exact; press only when idle", "[dsp][releasefx]")
{
    Runner run;
    for (int b = 0; b < 20; ++b)
    {
        std::vector<float> expected(kBlock);
        for (int i = 0; i < kBlock; ++i)
            expected[static_cast<std::size_t>(i)] =
                static_cast<float>(0.5 * std::sin(2.0 * 3.141592653589793 * 220.0 * (run.at + i) / kRate));
        const auto got = run.block();
        REQUIRE(std::memcmp(got.data(), expected.data(), sizeof(float) * kBlock) == 0);
    }
    run.fx.release(); // not held: nothing
    CHECK(run.fx.stage() == ReleaseFx::Stage::Idle);
    run.fx.press(ReleaseFx::Kind::Brake, 1.0);
    run.fx.press(ReleaseFx::Kind::EchoOut, 1.0); // already held: ignored
    CHECK(run.fx.kind() == ReleaseFx::Kind::Brake);
}

TEST_CASE("Release FX: echo out - the echo while held, the music cut on release, the echo rings out",
          "[dsp][releasefx]")
{
    Runner run;
    run.fx.press(ReleaseFx::Kind::EchoOut, 0.5);
    run.blocks(150); // held for ~0.8 s: music + echo
    run.fx.release();
    const auto after = run.blocks(200);
    // 10 ms after the release the music (220 Hz, 0.5) is gone; the echo of it is still there for a while...
    CHECK(rms(after, 2000, 8000) > 0.02);
    // ...and dies away, then the stage is Finished and the output silent.
    int extra = 0;
    while (!run.fx.finished() && extra < 3000)
    {
        run.block();
        ++extra;
    }
    REQUIRE(run.fx.finished());
    const auto silent = run.blocks(10);
    CHECK(rms(silent, 0, silent.size()) == 0.0);
    run.fx.reset();
    CHECK(run.fx.stage() == ReleaseFx::Stage::Idle);
}

TEST_CASE("Release FX: brake slows to a stop in the beat length; spin back goes backwards and stops; sweep fades",
          "[dsp][releasefx]")
{
    SECTION("brake")
    {
        Runner run;
        run.blocks(100);
        run.fx.press(ReleaseFx::Kind::Brake, 1.0);
        const auto held = run.blocks(40);
        CHECK(rms(held, 0, held.size()) > 0.3); // held: the music as it is
        run.fx.release();
        const auto out = run.blocks(static_cast<int>(kBeat / kBlock) + 4);
        // Early on nearly the full speed, later much slower (the 220 Hz sine's crossings thin out).
        const double early = crossings(out, 0, 3000), late = crossings(out, static_cast<std::size_t>(0.75 * kBeat),
                                                                         static_cast<std::size_t>(0.85 * kBeat));
        CHECK(early > 0.8 * 2.0 * 220.0 / kRate);
        CHECK(late < 0.4 * early);
        CHECK(run.fx.finished());
        CHECK(rms(out, static_cast<std::size_t>(kBeat) + 10, out.size()) == 0.0);
    }
    SECTION("spin back")
    {
        Runner run;
        run.blocks(300); // 1.6 s of past to spin back through
        run.fx.press(ReleaseFx::Kind::SpinBack, 1.0);
        run.fx.release();
        const auto out = run.blocks(static_cast<int>(kBeat / kBlock) + 4);
        CHECK(crossings(out, 500, 3000) > 2.0 * 2.0 * 220.0 / kRate); // faster than the music (3x)
        CHECK(run.fx.finished());
    }
    SECTION("filter sweep")
    {
        Runner run;
        run.fx.press(ReleaseFx::Kind::FilterSweep, 1.0);
        const auto held = run.blocks(static_cast<int>(kBeat / kBlock), 100.0);
        // A 100 Hz tone: through at first, cut by the rising high-pass by the end of the beat.
        CHECK(rms(held, 0, 2000) > 0.25);
        CHECK(rms(held, held.size() - 2000, held.size()) < 0.05);
        run.fx.release();
        run.blocks(static_cast<int>(kBeat / kBlock) + 2, 3000.0);
        CHECK(run.fx.finished());
    }
}

TEST_CASE("Release FX in the engine: after the release the deck pauses and the channel is normal again",
          "[engine][releasefx]")
{
    engine::TrackBuffer track;
    track.sampleRate = kRate;
    track.audio.setSize(2, static_cast<int>(20.0 * kRate));
    for (int i = 0; i < track.audio.getNumSamples(); ++i)
    {
        const auto v = static_cast<float>(0.3 * std::sin(2.0 * 3.141592653589793 * 220.0 * i / kRate));
        track.audio.setSample(0, i, v);
        track.audio.setSample(1, i, v);
    }
    engine::AudioEngine e;
    e.prepare(kRate, kBlock);
    e.send(engine::LoadTrack{0, &track, 1});
    e.send(engine::Play{0});
    float l[kBlock], r[kBlock];
    float* outs[] = {l, r};
    for (int b = 0; b < 20; ++b)
        e.process(outs, 2, kBlock);
    e.send(engine::ReleaseFxCommand{0, static_cast<std::uint8_t>(ReleaseFx::Kind::Brake), true, 0.5f});
    for (int b = 0; b < 20; ++b)
        e.process(outs, 2, kBlock);
    CHECK(e.readState().decks[0].releaseFx == 1); // held
    e.send(engine::ReleaseFxCommand{0, 0, false, 0.0f});
    for (int b = 0; b < 100 && e.readState().decks[0].playing; ++b)
        e.process(outs, 2, kBlock);
    CHECK_FALSE(e.readState().decks[0].playing); // ended on the effect: paused
    e.process(outs, 2, kBlock);                  // the deck has faded out: the channel is normal again
    e.process(outs, 2, kBlock);
    CHECK(e.readState().decks[0].releaseFx == 0);
    // Play again: the music as normal.
    e.send(engine::Play{0});
    double peak = 0.0;
    for (int b = 0; b < 20; ++b)
    {
        e.process(outs, 2, kBlock);
        for (const float v : l)
            peak = std::max(peak, static_cast<double>(std::abs(v)));
    }
    CHECK(peak > 0.15); // the normal level (0.3 x the crossfader centre 0.71)
}
