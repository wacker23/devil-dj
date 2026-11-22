// P3-17: auto-transition. Done when: engine test renders a transition offline and checks crossfader/EQ positions
// per beat; manual listening test.

#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace devildj;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

struct Rig
{
    engine::TrackBuffer a, b;
    engine::AudioEngine engine;
    float l[kBlock], r[kBlock];
    float* outs[2] = {l, r};
    util::Beatgrid grid = util::Beatgrid::constant(0.0, 120.0, kRate); // a beat every 24000 samples, from 0
    Rig()
    {
        for (auto* t : {&a, &b})
        {
            t->sampleRate = kRate;
            t->audio.setSize(2, static_cast<int>(120.0 * kRate));
            for (int i = 0; i < t->audio.getNumSamples(); ++i)
            {
                const auto v = static_cast<float>(0.2 * std::sin(2.0 * 3.141592653589793 * (t == &a ? 220.0 : 330.0) * i / kRate));
                t->audio.setSample(0, i, v);
                t->audio.setSample(1, i, v);
            }
        }
        engine.prepare(kRate, kBlock);
        engine.send(engine::LoadTrack{0, &a, 1});
        engine.send(engine::LoadTrack{1, &b, 2});
        engine.send(engine::SetBeatgrid{0, grid});
        engine.send(engine::SetBeatgrid{1, grid});
        engine.send(engine::SetParam{engine::ParamId::Crossfader, 0.0f}); // on deck A's side
        engine.send(engine::Play{0});
        block();
    }
    void block() { engine.process(outs, 2, kBlock); }
    const engine::EngineState& state() { return engine.readState(); }
};

struct Snapshot
{
    double beat = 0.0;
    float crossfader = 0.0f, lowA = 0.0f, lowB = 0.0f, filterA = 0.5f;
    bool playingA = false, playingB = false, active = false;
};

// One snapshot at the first block of each master beat from `fromBeat` to `toBeat`.
std::vector<Snapshot> run(Rig& rig, int fromBeat, int toBeat)
{
    std::vector<Snapshot> out;
    for (int guard = 0; guard < 200000; ++guard)
    {
        rig.block();
        const auto& s = rig.state();
        // The first block that starts on or after each beat (masterBeat is where the block ended).
        const double beat = s.masterBeat - kBlock * 120.0 / (60.0 * kRate);
        if (beat >= fromBeat + static_cast<double>(out.size()) && out.size() < static_cast<std::size_t>(toBeat - fromBeat + 1))
            out.push_back({beat, s.crossfader, s.channels[0].eqDb[0], s.channels[1].eqDb[0], s.channels[0].filter,
                           s.decks[0].playing, s.decks[1].playing, s.transition.active});
        if (out.size() == static_cast<std::size_t>(toBeat - fromBeat + 1))
            break;
    }
    return out;
}
} // namespace

TEST_CASE("Auto-transition: 16 beats from the next 16-beat boundary - crossfader ramp, low EQ swap at the middle",
          "[engine][transition]")
{
    Rig rig;
    while (rig.state().masterBeat < 3.2) // somewhere in the first 16 beats
        rig.block();
    rig.engine.send(engine::StartTransition{0, 1, 16.0f, engine::TransitionStyle::Blend, -1.0});
    rig.block();
    REQUIRE(rig.state().transition.active);
    CHECK(rig.state().transition.startBeat == 16.0); // the next 16-beat boundary
    CHECK(std::isinf(rig.state().channels[1].eqDb[0])); // the incoming deck's lows killed while it waits
    CHECK_FALSE(rig.state().decks[1].playing);
    const auto beats = run(rig, 15, 33);
    REQUIRE(beats.size() == 19);
    // Beat 15: not yet. Beats 16..32: the transition, beat by beat.
    CHECK(beats[0].crossfader == 0.0f);
    CHECK_FALSE(beats[0].playingB);
    for (int k = 0; k <= 16; ++k)
    {
        const auto& s = beats[static_cast<std::size_t>(k + 1)];
        INFO("transition beat " << k << " (master " << s.beat << ")");
        const double into = s.beat - 16.0; // the snapshot's own position (its block's start)
        if (k < 16)
        {
            CHECK(s.playingB);                                            // started on beat 16, with sync
            CHECK(std::abs(s.crossfader - into / 16.0) < 0.002);          // a straight ramp A -> B
            if (into < 8.0)
            {
                CHECK(s.lowA == 0.0f);         // first half: A's lows, B's killed
                CHECK(std::isinf(s.lowB));
            }
            else
            {
                CHECK(std::isinf(s.lowA));     // the swap at the middle
                CHECK(s.lowB == 0.0f);
            }
        }
    }
    // The end: on B's side, A stopped, its lows back, the transition over.
    const auto& end = beats.back();
    CHECK(end.crossfader == 1.0f);
    CHECK_FALSE(end.playingA);
    CHECK(end.playingB);
    CHECK(end.lowA == 0.0f);
    CHECK(end.lowB == 0.0f);
    CHECK_FALSE(end.active);
    CHECK(rig.state().decks[1].synced);
}

TEST_CASE("Auto-transition: a filter sweep out, an echo out, and cancelling with T again", "[engine][transition]")
{
    SECTION("filter sweep: A's filter rises to a high-pass and comes back after")
    {
        Rig rig;
        rig.engine.send(engine::StartTransition{0, 1, 8.0f, engine::TransitionStyle::FilterSweep, 4.0});
        const auto beats = run(rig, 4, 13);
        CHECK(beats[0].filterA < 0.52f);
        CHECK(beats[4].filterA > 0.7f); // half way: well into the high-pass
        CHECK(beats[9].filterA == 0.5f);
        CHECK_FALSE(beats[9].playingA);
    }
    SECTION("echo out: A ends on an echo (its release FX), then pauses")
    {
        Rig rig;
        rig.engine.send(engine::StartTransition{0, 1, 8.0f, engine::TransitionStyle::EchoOut, 4.0});
        const auto beats = run(rig, 4, 11);
        CHECK((rig.state().decks[0].releaseFx != 0 || !rig.state().decks[0].playing));
        run(rig, 12, 40); // the echo rings out (feedback 0.65: ~11 s at 120) before the deck pauses
        CHECK_FALSE(rig.state().decks[0].playing);
        CHECK(rig.state().decks[1].playing);
    }
    SECTION("cancel: the automation stops where it is, the EQs come back")
    {
        Rig rig;
        rig.engine.send(engine::StartTransition{0, 1, 16.0f, engine::TransitionStyle::Blend, 4.0});
        run(rig, 4, 14); // past the swap
        CHECK(std::isinf(rig.state().channels[0].eqDb[0]));
        const float xf = rig.state().crossfader;
        rig.engine.send(engine::CancelTransition{});
        rig.block();
        CHECK_FALSE(rig.state().transition.active);
        CHECK(rig.state().channels[0].eqDb[0] == 0.0f);
        CHECK(rig.state().channels[1].eqDb[0] == 0.0f);
        run(rig, 16, 20);
        CHECK(std::abs(rig.state().crossfader - xf) < 0.01f); // left where it was
        CHECK(rig.state().decks[0].playing);                   // nobody stopped
    }
    SECTION("cancelled before it starts: the incoming deck doesn't start")
    {
        Rig rig;
        rig.engine.send(engine::StartTransition{0, 1, 16.0f, engine::TransitionStyle::Blend, 8.0});
        rig.block();
        rig.engine.send(engine::CancelTransition{});
        run(rig, 8, 10);
        CHECK_FALSE(rig.state().decks[1].playing);
        CHECK(rig.state().channels[1].eqDb[0] == 0.0f);
    }
}
