// P3-01: hot cues in the engine - set, trigger (quantized or not), hold from pause, delete, load. Storage and
// the keys: tests/app/HotCueStoreTests.cpp.

#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace devildj;
using engine::HotCueOp;

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
    util::Beatgrid grid = util::Beatgrid::constant(0.5 * kRate, 128.0, kRate);
    Rig()
    {
        track.sampleRate = kRate;
        track.audio.setSize(2, static_cast<int>(120 * kRate));
        track.audio.clear();
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
    void send(std::uint8_t slot, HotCueOp op)
    {
        engine.send(engine::HotCue{0, slot, op});
        block();
    }
    const engine::DeckState& deck() { return engine.readState().decks[0]; }
};
} // namespace

TEST_CASE("Hot cues: a press on an empty pad sets it at the playhead; quantize snaps it", "[engine][hotcue]")
{
    Rig rig;
    for (int c = 0; c < engine::kHotCues; ++c)
        CHECK(rig.deck().hotCueStart[static_cast<std::size_t>(c)] < 0.0); // all empty
    rig.engine.send(engine::Seek{0, 3.21 * kRate});
    rig.send(0, HotCueOp::Press);
    CHECK(rig.deck().hotCueStart[0] == 3.21 * kRate);
    CHECK(rig.deck().hotCueLength[0] == 0.0);
    CHECK_FALSE(rig.deck().playing); // setting doesn't start anything
    // Quantize: the nearest beat (3.6 s = beat 6.61 -> beat 7, at 0.5 + 7 x 60 / 128 s).
    rig.engine.send(engine::SetQuantize{true, 1.0f});
    rig.engine.send(engine::Seek{0, 3.6 * kRate});
    rig.block();
    rig.send(15, HotCueOp::Press); // the last pad (page 2, P)
    CHECK(std::abs(rig.deck().hotCueStart[15] - rig.grid.sampleAt(7.0)) < 1e-6);
    // Delete.
    rig.send(0, HotCueOp::Delete);
    CHECK(rig.deck().hotCueStart[0] < 0.0);
    CHECK(rig.deck().hotCueStart[15] >= 0.0);
}

TEST_CASE("Hot cues: playing, a press jumps there - at once, or with quantize on the next beat", "[engine][hotcue]")
{
    Rig rig;
    engine::SetHotCues load{0, {}};
    load.cues[2] = {true, rig.grid.sampleAt(32.0), 0.0}; // pad C on beat 32
    rig.engine.send(load);
    rig.engine.send(engine::Seek{0, rig.grid.sampleAt(10.3)});
    rig.engine.send(engine::Play{0});
    rig.blocks(2);
    CHECK(rig.deck().hotCueStart[2] == rig.grid.sampleAt(32.0));
    // Not quantized: straight there (and on playing from it).
    rig.send(2, HotCueOp::Press);
    CHECK(rig.deck().playing);
    CHECK(std::abs(rig.deck().positionSamples - (rig.grid.sampleAt(32.0) + kBlock)) <= 1.0);
    CHECK(rig.deck().hotCuePreview == -1); // playing: no hold
    // Quantized: on the next beat, landing on the cue + what was left of the block (the phase carries on).
    rig.engine.send(engine::Seek{0, rig.grid.sampleAt(40.3)});
    rig.engine.send(engine::SetQuantize{true, 1.0f});
    rig.block();
    rig.send(2, HotCueOp::Press);
    double before = rig.deck().positionSamples;
    bool jumped = false;
    for (int i = 0; i < 200 && !jumped; ++i)
    {
        rig.block();
        const double now = rig.deck().positionSamples;
        if (now < before)
        {
            jumped = true;
            const double crossing = rig.grid.sampleAt(41.0);
            REQUIRE(before < crossing);
            REQUIRE(before + kBlock >= crossing);
            CHECK(std::abs(now - (rig.grid.sampleAt(32.0) + (before + kBlock - crossing))) <= 1.0);
        }
        before = now;
    }
    CHECK(jumped);

    // Two quantized jumps waiting for the same beat (a seek, then a hot cue): both happen there, in order - the
    // second lands on the cue, not past it.
    rig.engine.send(engine::Seek{0, rig.grid.sampleAt(60.0)}); // quantize on: waits for the next beat
    rig.engine.send(engine::HotCue{0, 2, HotCueOp::Press});
    rig.blocks(150);
    CHECK(rig.deck().queuedActions == 0);
    CHECK(rig.deck().positionSamples > rig.grid.sampleAt(32.0));
    CHECK(rig.deck().positionSamples < rig.grid.sampleAt(32.0) + 151 * kBlock + 2.0);
}

TEST_CASE("Hot cues: paused, a held pad plays from it; release goes back; Play takes over", "[engine][hotcue]")
{
    Rig rig;
    engine::SetHotCues load{0, {}};
    load.cues[0] = {true, 10.0 * kRate, 0.0};
    rig.engine.send(load);
    rig.engine.send(engine::Seek{0, 2.0 * kRate});
    rig.block();
    // Hold: plays from the cue.
    rig.engine.send(engine::HotCue{0, 0, HotCueOp::Press});
    rig.blocks(40);
    CHECK(rig.deck().playing);
    CHECK(rig.deck().hotCuePreview == 0);
    CHECK(rig.deck().positionSamples > 10.0 * kRate);
    // Release: back on the cue, paused.
    rig.send(0, HotCueOp::Release);
    CHECK_FALSE(rig.deck().playing);
    CHECK(rig.deck().positionSamples == 10.0 * kRate);
    CHECK(rig.deck().hotCuePreview == -1);
    // Hold, then Play while holding, then release: it keeps playing.
    rig.engine.send(engine::HotCue{0, 0, HotCueOp::Press});
    rig.blocks(4);
    rig.engine.send(engine::Play{0});
    rig.blocks(4);
    rig.send(0, HotCueOp::Release);
    rig.blocks(4);
    CHECK(rig.deck().playing);
    CHECK(rig.deck().positionSamples > 10.0 * kRate);
    // The main cue point didn't move (hot cues are separate).
    CHECK(rig.deck().cuePointSamples == 0.0);
    // A new track: no hot cues until the app sends its own.
    rig.engine.send(engine::LoadTrack{0, &rig.track, 2});
    rig.blocks(30);
    CHECK(rig.deck().hotCueStart[0] < 0.0);
}
