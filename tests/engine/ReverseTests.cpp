// P3-05: reverse, vinyl brake and spin-back. Done when: manual test (these cover the engine side).

#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace devildj;
using engine::LoopOp;
using engine::StopKind;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

struct Rig
{
    engine::TrackBuffer track;
    engine::AudioEngine engine;
    float l[kBlock], r[kBlock];
    float* outs[2] = {l, r};
    util::Beatgrid grid = util::Beatgrid::constant(0.5 * kRate, 120.0, kRate);
    explicit Rig(double hz = 0.0)
    {
        track.sampleRate = kRate;
        track.audio.setSize(2, static_cast<int>(60.0 * kRate));
        for (int i = 0; i < track.audio.getNumSamples(); ++i)
        {
            const auto v = hz > 0.0 ? static_cast<float>(0.5 * std::sin(2.0 * 3.141592653589793 * hz * i / kRate)) : 0.0f;
            track.audio.setSample(0, i, v);
            track.audio.setSample(1, i, v);
        }
        engine.prepare(kRate, kBlock);
        engine.send(engine::LoadTrack{0, &track, 1});
        engine.send(engine::SetBeatgrid{0, grid});
        block();
    }
    void block() { engine.process(outs, 2, kBlock); }
    void blocks(int n)
    {
        for (int i = 0; i < n; ++i)
            block();
    }
    void playFrom(double at)
    {
        engine.send(engine::Seek{0, at});
        engine.send(engine::Play{0});
        block();
    }
    const engine::DeckState& deck() { return engine.readState().decks[0]; }
};
} // namespace

TEST_CASE("Reverse: backwards at the rate; stops at the start; loops wrap back to their end", "[engine][reverse]")
{
    Rig rig;
    SECTION("plain, then with key lock and +5 %")
    {
        for (const bool keyLock : {false, true})
        {
            INFO("key lock " << keyLock);
            rig.engine.send(engine::SetKeyLock{0, keyLock});
            rig.engine.send(engine::SetTempo{0, keyLock ? 5.0 : 0.0});
            rig.playFrom(10.0 * kRate);
            const double from = rig.deck().positionSamples;
            rig.engine.send(engine::SetReverse{0, true});
            rig.blocks(100);
            CHECK(rig.deck().reverse);
            const double rate = keyLock ? 1.05 : 1.0;
            CHECK(std::abs(rig.deck().positionSamples - (from - 100.0 * kBlock * rate)) < 1e-6);
            rig.engine.send(engine::SetReverse{0, false});
            rig.blocks(10);
            CHECK(std::abs(rig.deck().positionSamples - (from - 90.0 * kBlock * rate)) < 1e-6);
            rig.engine.send(engine::Pause{0});
            rig.block();
        }
    }
    SECTION("at the start it stops")
    {
        rig.engine.send(engine::SetReverse{0, true});
        rig.playFrom(1000.0);
        rig.blocks(10);
        CHECK(rig.deck().positionSamples == 0.0);
        CHECK_FALSE(rig.deck().playing);
    }
    SECTION("in a loop")
    {
        rig.playFrom(rig.grid.sampleAt(20.5));
        rig.engine.send(engine::Loop{0, LoopOp::Set, 0.0, rig.grid.sampleAt(20.0), rig.grid.sampleAt(21.0)});
        rig.engine.send(engine::SetReverse{0, true});
        double lowest = 1e18, highest = 0.0;
        int wraps = 0;
        double previous = rig.deck().positionSamples;
        for (int b = 0; b < 400; ++b) // ~2 s: four times round
        {
            rig.block();
            const double p = rig.deck().positionSamples;
            lowest = std::min(lowest, p);
            highest = std::max(highest, p);
            wraps += p > previous ? 1 : 0;
            previous = p;
        }
        CHECK(rig.deck().loopActive);
        CHECK(lowest >= rig.grid.sampleAt(20.0) - 1.0); // (a block can end just past it: it wraps next sample)
        CHECK(highest < rig.grid.sampleAt(21.0));
        CHECK(wraps >= 3);
    }
}

TEST_CASE("Reverse with slip returns to where the track would be (censor)", "[engine][reverse][slip]")
{
    Rig rig;
    rig.engine.send(engine::SetSlip{0, true});
    rig.playFrom(10.0 * kRate);
    rig.engine.send(engine::SetReverse{0, true});
    rig.blocks(150);
    CHECK(rig.deck().slipping);
    CHECK(rig.deck().positionSamples < 10.0 * kRate);
    rig.engine.send(engine::SetReverse{0, false});
    rig.block();
    CHECK_FALSE(rig.deck().slipping);
    CHECK(rig.deck().positionSamples == 10.0 * kRate + 152.0 * kBlock);
}

TEST_CASE("Brake and spin-back: they slow to a stop in the set time; play takes the deck back", "[engine][reverse]")
{
    Rig rig;
    rig.engine.send(engine::SetStopStyle{StopKind::Instant, 0.5f, 1.0f});
    SECTION("brake: linear, half the distance of playing on, then paused")
    {
        rig.playFrom(10.0 * kRate);
        const double from = rig.deck().positionSamples;
        rig.engine.send(engine::Stop{0, StopKind::Brake});
        rig.block();
        CHECK(rig.deck().stopping == 1);
        CHECK(rig.deck().playing);
        rig.blocks(46); // 0.25 s in
        CHECK(std::abs(rig.deck().rate - 0.5) < 0.02);
        rig.blocks(50); // past 0.5 s
        CHECK_FALSE(rig.deck().playing);
        CHECK(rig.deck().stopping == 0);
        const double travelled = rig.deck().positionSamples - from;
        CHECK(std::abs(travelled - 0.25 * kRate) < 0.002 * kRate); // the area under a 1 -> 0 ramp over 0.5 s
    }
    SECTION("spin-back: backwards, slowing, then paused")
    {
        rig.playFrom(10.0 * kRate);
        const double from = rig.deck().positionSamples;
        rig.engine.send(engine::Stop{0, StopKind::SpinBack});
        rig.blocks(20); // ~0.1 s: past the 60 ms push
        CHECK(rig.deck().rate < -2.0);
        rig.blocks(200);
        CHECK_FALSE(rig.deck().playing);
        CHECK(rig.deck().positionSamples < from - 0.5 * kRate); // it went back about a second's worth
    }
    SECTION("play during a brake: back to the normal rate; with slip, to the shadow")
    {
        rig.playFrom(10.0 * kRate);
        rig.engine.send(engine::Stop{0, StopKind::Brake});
        rig.blocks(40);
        rig.engine.send(engine::Play{0});
        rig.blocks(2);
        CHECK(rig.deck().stopping == 0);
        CHECK(rig.deck().rate == 1.0);
        rig.engine.send(engine::SetSlip{0, true});
        rig.engine.send(engine::Stop{0, StopKind::SpinBack});
        rig.block();
        const double shadowFrom = rig.deck().slipPositionSamples;
        rig.blocks(60);
        CHECK(rig.deck().slipping);
        rig.engine.send(engine::TogglePlay{0}); // during the stop: plays again
        rig.block();
        CHECK(rig.deck().playing);
        CHECK_FALSE(rig.deck().slipping);
        CHECK(rig.deck().positionSamples == shadowFrom + 61.0 * kBlock);
    }
    SECTION("PAUSE with the brake style; a paused deck isn't braked")
    {
        rig.engine.send(engine::SetStopStyle{StopKind::Brake, 0.5f, 1.0f});
        rig.playFrom(10.0 * kRate);
        rig.engine.send(engine::TogglePlay{0});
        rig.block();
        CHECK(rig.deck().stopping == 1);
        rig.blocks(100);
        CHECK_FALSE(rig.deck().playing);
        rig.engine.send(engine::Stop{0, StopKind::Brake});
        rig.block();
        CHECK(rig.deck().stopping == 0);
        CHECK_FALSE(rig.deck().playing);
    }
}

TEST_CASE("Reverse and stops: no clicks (sine)", "[engine][reverse]")
{
    // Turning round, braking to a stop and spinning back keep the wave continuous: the largest sample-to-sample
    // step stays within a few times the sine's own (a spin-back plays it 3x as fast).
    Rig rig(200.0);
    rig.engine.send(engine::SetStopStyle{StopKind::Instant, 0.5f, 0.5f});
    rig.playFrom(10.0 * kRate);
    rig.blocks(4);
    const double slope = 2.0 * 3.141592653589793 * 200.0 / kRate * 0.5;
    double worst = 0.0;
    float previous = rig.l[kBlock - 1];
    const auto run = [&](int n)
    {
        for (int b = 0; b < n; ++b)
        {
            rig.block();
            for (int i = 0; i < kBlock; ++i)
            {
                worst = std::max(worst, static_cast<double>(std::abs(rig.l[i] - previous)));
                previous = rig.l[i];
            }
        }
    };
    rig.engine.send(engine::SetReverse{0, true});
    run(50);
    rig.engine.send(engine::SetReverse{0, false});
    run(50);
    rig.engine.send(engine::Stop{0, StopKind::Brake});
    run(120);
    const double afterBrake = worst;
    rig.engine.send(engine::Play{0});
    run(20);
    rig.engine.send(engine::Stop{0, StopKind::SpinBack});
    run(120);
    WARN("largest step: " << afterBrake / slope << " x the sine's own (reverse, brake); " << worst / slope
                          << " x (with the spin-back)");
    CHECK(afterBrake < 1.6 * slope);
    CHECK(worst < 3.5 * slope);
}
