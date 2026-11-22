// P3-04: slip mode. Done when: engine test: slip loop of 1 beat for 4 beats returns to exactly +4 beats.

#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace devildj;
using engine::LoopOp;

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
    util::Beatgrid grid = util::Beatgrid::constant(0.5 * kRate, 120.0, kRate); // a beat every 24000 samples
    Rig()
    {
        track.sampleRate = kRate;
        track.audio.setSize(2, static_cast<int>(120.0 * kRate));
        track.audio.clear();
        engine.prepare(kRate, kBlock);
        engine.send(engine::LoadTrack{0, &track, 1});
        engine.send(engine::SetBeatgrid{0, grid});
        block();
        engine.send(engine::SetSlip{0, true});
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
    double beat(double b) const { return grid.sampleAt(b); }
};
} // namespace

TEST_CASE("Slip: a 1-beat loop held for 4 beats returns to exactly +4 beats", "[engine][slip]")
{
    Rig rig;
    rig.engine.send(engine::SetQuantize{true, 1.0f});
    rig.engine.send(engine::Seek{0, rig.beat(10.0)});
    rig.engine.send(engine::Play{0});
    rig.engine.send(engine::Loop{0, LoopOp::Auto, 1.0}); // a 1-beat loop from beat 10, slipping
    rig.block();
    REQUIRE(rig.deck().loopActive);
    CHECK(rig.deck().slipping);
    // 4 beats = 96000 samples = 375 blocks (one played already).
    double lowest = 1e18, highest = 0.0;
    for (int b = 1; b < 375; ++b)
    {
        rig.block();
        lowest = std::min(lowest, rig.deck().positionSamples);
        highest = std::max(highest, rig.deck().positionSamples);
    }
    CHECK(lowest >= rig.beat(10.0));
    CHECK(highest <= rig.beat(11.0)); // looped all the while (it wraps at the next sample)
    CHECK(rig.deck().slipPositionSamples == rig.beat(14.0));
    rig.engine.send(engine::Loop{0, LoopOp::Exit});
    rig.block();
    CHECK_FALSE(rig.deck().loopActive);
    CHECK_FALSE(rig.deck().slipping);
    CHECK(rig.deck().positionSamples == rig.beat(14.0) + kBlock); // exactly where the track would be
    CHECK(rig.deck().playing);
}

TEST_CASE("Slip: the shadow keeps the tempo (+3.7 %, key lock -2.5 %)", "[engine][slip]")
{
    for (const auto [tempo, keyLock] : {std::pair{3.7, false}, std::pair{-2.5, true}})
    {
        INFO("tempo " << tempo << " %, key lock " << keyLock);
        Rig rig;
        rig.engine.send(engine::SetTempo{0, tempo});
        rig.engine.send(engine::SetKeyLock{0, keyLock});
        rig.playFrom(rig.beat(20.0));
        const double from = rig.deck().positionSamples;
        rig.engine.send(engine::Loop{0, LoopOp::Set, 0.0, rig.beat(20.0), rig.beat(20.5)});
        rig.blocks(300);
        rig.engine.send(engine::Loop{0, LoopOp::Exit});
        rig.block();
        const double rate = 1.0 + tempo / 100.0;
        CHECK(std::abs(rig.deck().positionSamples - (from + 301.0 * kBlock * rate)) < 1e-4);
    }
}

TEST_CASE("Slip: a held hot cue, scratching and pause return to the shadow", "[engine][slip][hotcue]")
{
    Rig rig;
    // Hot cue A at beat 2.
    rig.engine.send(engine::Seek{0, rig.beat(2.0)});
    rig.engine.send(engine::HotCue{0, 0, engine::HotCueOp::Press});
    rig.engine.send(engine::HotCue{0, 0, engine::HotCueOp::Release});
    rig.block();
    REQUIRE(rig.deck().hotCueStart[0] == rig.beat(2.0));

    SECTION("hot cue held")
    {
        rig.playFrom(rig.beat(20.0));
        rig.engine.send(engine::HotCue{0, 0, engine::HotCueOp::Press});
        rig.block();
        CHECK(rig.deck().slipping);
        CHECK(rig.deck().positionSamples == rig.beat(2.0) + kBlock); // playing from the cue
        rig.blocks(100);
        rig.engine.send(engine::HotCue{0, 0, engine::HotCueOp::Release});
        rig.block();
        CHECK_FALSE(rig.deck().slipping);
        CHECK(rig.deck().positionSamples == rig.beat(20.0) + 103.0 * kBlock);
    }
    SECTION("hot cue held, quantized, released before its beat: no jump")
    {
        rig.playFrom(rig.beat(20.1)); // (a quantized seek would snap to the beat)
        rig.engine.send(engine::SetQuantize{true, 1.0f});
        rig.engine.send(engine::HotCue{0, 0, engine::HotCueOp::Press});
        rig.block();
        CHECK(rig.deck().queuedActions == 1);
        rig.engine.send(engine::HotCue{0, 0, engine::HotCueOp::Release});
        rig.block();
        CHECK(rig.deck().queuedActions == 0);
        rig.blocks(100); // past beat 21
        CHECK(rig.deck().positionSamples == rig.beat(20.1) + 103.0 * kBlock);
    }
    SECTION("scratch")
    {
        rig.playFrom(rig.beat(20.0));
        rig.engine.send(engine::ScratchBegin{0});
        for (int i = 0; i < 60; ++i)
        {
            rig.engine.send(engine::ScratchMove{0, -400.0}); // backwards
            rig.block();
        }
        CHECK(rig.deck().slipping);
        CHECK(rig.deck().positionSamples < rig.beat(20.0));
        rig.engine.send(engine::ScratchEnd{0});
        rig.block();
        CHECK_FALSE(rig.deck().slipping);
        CHECK_FALSE(rig.deck().scratching);
        CHECK(rig.deck().positionSamples == rig.beat(20.0) + 62.0 * kBlock);
    }
    SECTION("pause")
    {
        rig.playFrom(rig.beat(20.0));
        rig.engine.send(engine::Pause{0});
        rig.blocks(50);
        CHECK(rig.deck().slipping);
        CHECK_FALSE(rig.deck().playing);
        CHECK(rig.deck().positionSamples == rig.beat(20.0) + kBlock);
        rig.engine.send(engine::Play{0});
        rig.block();
        CHECK(rig.deck().positionSamples == rig.beat(20.0) + 52.0 * kBlock);
        // A seek while slip-paused: play goes on from there instead.
        rig.engine.send(engine::Pause{0});
        rig.block();
        rig.engine.send(engine::Seek{0, rig.beat(40.0)});
        rig.blocks(10);
        rig.engine.send(engine::Play{0});
        rig.block();
        CHECK(rig.deck().positionSamples == rig.beat(40.0) + kBlock);
    }
}

TEST_CASE("Slip: off = no return; slip off while slipping stays; a seek out drops the shadow", "[engine][slip]")
{
    Rig rig;
    SECTION("slip off: a loop exit plays on from the loop")
    {
        rig.engine.send(engine::SetSlip{0, false});
        rig.playFrom(rig.beat(20.0));
        rig.engine.send(engine::Loop{0, LoopOp::Set, 0.0, rig.beat(20.0), rig.beat(21.0)});
        rig.blocks(150);
        CHECK_FALSE(rig.deck().slipping);
        rig.engine.send(engine::Loop{0, LoopOp::Exit});
        rig.block();
        CHECK(rig.deck().positionSamples < rig.beat(21.0) + kBlock);
    }
    SECTION("slip turned off mid-loop")
    {
        rig.playFrom(rig.beat(20.0));
        rig.engine.send(engine::Loop{0, LoopOp::Set, 0.0, rig.beat(20.0), rig.beat(21.0)});
        rig.blocks(150);
        rig.engine.send(engine::SetSlip{0, false});
        rig.block();
        CHECK_FALSE(rig.deck().slipping);
        rig.engine.send(engine::Loop{0, LoopOp::Exit});
        rig.block();
        CHECK(rig.deck().positionSamples < rig.beat(21.0) + kBlock);
    }
    SECTION("a seek out of a slip loop")
    {
        rig.playFrom(rig.beat(20.0));
        rig.engine.send(engine::Loop{0, LoopOp::Set, 0.0, rig.beat(20.0), rig.beat(21.0)});
        rig.blocks(150);
        rig.engine.send(engine::Seek{0, rig.beat(60.0)});
        rig.block();
        CHECK_FALSE(rig.deck().loopActive);
        CHECK_FALSE(rig.deck().slipping);
        CHECK(rig.deck().positionSamples == rig.beat(60.0) + kBlock);
    }
    SECTION("a quantized hot loop slips too")
    {
        rig.engine.send(engine::SetQuantize{true, 1.0f});
        rig.playFrom(rig.beat(20.0));
        rig.engine.send(engine::Loop{0, LoopOp::Auto, 1.0}); // beats 20..21: slipping
        rig.engine.send(engine::HotCue{0, 3, engine::HotCueOp::Press}); // stored as hot loop D
        rig.block();
        rig.engine.send(engine::Loop{0, LoopOp::Exit}); // back to the shadow (beat 20 + 2 blocks)
        rig.block();
        CHECK(rig.deck().positionSamples == rig.beat(20.0) + 3.0 * kBlock);
        const double pressedAt = rig.deck().positionSamples;
        rig.engine.send(engine::HotCue{0, 3, engine::HotCueOp::Press}); // loops from the next beat
        rig.blocks(200);
        CHECK(rig.deck().loopActive);
        CHECK(rig.deck().slipping);
        rig.engine.send(engine::Loop{0, LoopOp::Exit});
        rig.block();
        CHECK(rig.deck().positionSamples == pressedAt + 201.0 * kBlock);
    }
}
