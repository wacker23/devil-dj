// P2-20: quantize. Done when: engine test: triggered actions land on beat +-1 sample.

#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>

using namespace devildj;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

std::unique_ptr<engine::TrackBuffer> silentTrack(double seconds)
{
    auto t = std::make_unique<engine::TrackBuffer>();
    t->sampleRate = kRate;
    t->audio.setSize(2, static_cast<int>(seconds * kRate));
    t->audio.clear();
    return t;
}

struct Rig
{
    engine::AudioEngine engine;
    float l[kBlock], r[kBlock];
    float* outs[2] = {l, r};
    std::int64_t rendered = 0;
    Rig() { engine.prepare(kRate, kBlock); }
    void block()
    {
        engine.process(outs, 2, kBlock);
        rendered += kBlock;
    }
    void blocks(int n)
    {
        for (int i = 0; i < n; ++i)
            block();
    }
    const engine::DeckState& deck(int i) { return engine.readState().decks[static_cast<std::size_t>(i)]; }
};
} // namespace

TEST_CASE("Quantize: play from pause starts on the master's next grid point, +-1 sample", "[engine][quantize]")
{
    const auto trackA = silentTrack(120.0);
    const auto trackB = silentTrack(120.0);
    const auto gridA = util::Beatgrid::constant(0.5 * kRate, 128.0, kRate); // A: the master
    const auto gridB = util::Beatgrid::constant(0.3 * kRate, 124.0, kRate);
    const double spb = 60.0 / 128.0 * kRate;

    for (const double resolution : {1.0, 0.5, 0.25, 0.125})
        for (const int wait : {37, 101, 230}) // blocks before B is started: anywhere in a beat
        {
            INFO(resolution << " beat, play after " << wait << " blocks");
            Rig rig;
            rig.engine.send(engine::LoadTrack{0, trackA.get(), 1});
            rig.engine.send(engine::SetBeatgrid{0, gridA});
            rig.engine.send(engine::LoadTrack{1, trackB.get(), 2});
            rig.engine.send(engine::SetBeatgrid{1, gridB});
            rig.engine.send(engine::Seek{1, gridB.sampleAt(4.0)}); // B paused on its beat 4
            rig.engine.send(engine::SetQuantize{true, static_cast<float>(resolution)});
            rig.engine.send(engine::Play{0});
            rig.block(); // A starts at engine sample 0 (from position 0), at rate 1: position = engine sample
            rig.blocks(wait - 1);
            const double pressedAt = static_cast<double>(rig.rendered); // the command applies at the next block
            rig.engine.send(engine::Play{1});
            rig.block();
            CHECK(rig.deck(0).master);
            // B waits (unless the grid point is in this very block), then starts.
            int guard = 0;
            while (!rig.deck(1).playing && ++guard < 1000)
            {
                CHECK(rig.deck(1).queuedActions == 1);
                rig.block();
            }
            REQUIRE(rig.deck(1).playing);
            CHECK(rig.deck(1).queuedActions == 0);
            // When it started: B moves 1 sample per sample from its beat 4.
            const double startedAt = static_cast<double>(rig.rendered) - (rig.deck(1).positionSamples - gridB.sampleAt(4.0));
            // The master's grid points at this resolution: 0.5 s + k x resolution beats (A's position = time).
            const double k = std::ceil((pressedAt - 0.5 * kRate) / (spb * resolution) - 1e-9);
            const double expected = 0.5 * kRate + k * spb * resolution;
            CHECK(std::abs(startedAt - expected) <= 1.0);
            CHECK(startedAt >= pressedAt);
        }
}

TEST_CASE("Quantize: a cue set while paused snaps to the nearest grid point", "[engine][quantize]")
{
    const auto track = silentTrack(30.0);
    const auto grid = util::Beatgrid::constant(0.25 * kRate, 120.0, kRate); // a beat every 0.5 s
    Rig rig;
    rig.engine.send(engine::LoadTrack{0, track.get(), 1});
    rig.engine.send(engine::SetBeatgrid{0, grid});
    rig.engine.send(engine::SetQuantize{true, 0.25f});
    rig.engine.send(engine::Seek{0, 1.234 * kRate}); // quantize on: a seek while paused lands on the grid too
    rig.block();
    // 1.234 s = beat 1.968 -> 2.0 at 1/4 beat.
    CHECK(rig.deck(0).positionSamples == grid.sampleAt(2.0));
    rig.engine.send(engine::SetQuantize{false, 0.25f});
    rig.engine.send(engine::Seek{0, 1.234 * kRate});
    rig.block();
    CHECK(rig.deck(0).positionSamples == 1.234 * kRate);
    rig.engine.send(engine::SetQuantize{true, 0.25f});
    rig.engine.send(engine::CuePress{0});
    rig.block();
    CHECK(rig.deck(0).cuePointSamples == grid.sampleAt(2.0));
    rig.engine.send(engine::CueRelease{0});
    rig.block();
    CHECK(rig.deck(0).positionSamples == grid.sampleAt(2.0));
    // Off: the cue goes exactly where the playhead is.
    rig.engine.send(engine::SetQuantize{false, 1.0f});
    rig.engine.send(engine::Seek{0, 3.333 * kRate});
    rig.engine.send(engine::CuePress{0});
    rig.engine.send(engine::CueRelease{0});
    rig.block();
    CHECK(rig.deck(0).cuePointSamples == 3.333 * kRate);
}

TEST_CASE("Quantize: a jump while playing happens on the grid point and keeps the phase, +-1 sample",
          "[engine][quantize]")
{
    const auto track = silentTrack(120.0);
    const auto grid = util::Beatgrid::constant(0.5 * kRate, 128.0, kRate);
    const double spb = 60.0 / 128.0 * kRate;
    for (const double resolution : {1.0, 0.25})
        for (const double tempo : {0.0, 3.7}) // rate 1, and a rate where crossings fall between samples
        {
            INFO(resolution << " beat, tempo " << tempo << " %");
            Rig rig;
            rig.engine.send(engine::LoadTrack{0, track.get(), 1});
            rig.engine.send(engine::SetBeatgrid{0, grid});
            rig.engine.send(engine::SetTempo{0, tempo});
            rig.engine.send(engine::Play{0});
            rig.blocks(83);
            const double rate = 1.0 + tempo / 100.0;
            const double at = rig.deck(0).positionSamples;
            // Jump to somewhere near beat 40 (lands exactly on it).
            rig.engine.send(engine::Seek{0, grid.sampleAt(40.0) + 0.1 * spb * resolution, true});
            const double crossing =
                grid.sampleAt((std::floor(grid.beatAt(at) / resolution + 1e-9) + 1.0) * resolution);
            double before = at;
            bool jumped = false;
            for (int i = 0; i < 200 && !jumped; ++i)
            {
                rig.block();
                const double now = rig.deck(0).positionSamples;
                if (now < before || now > before + kBlock * rate + 1.0) // the jump came in this block
                {
                    jumped = true;
                    // Without it the playhead would be at before + block x rate; it crossed the grid point
                    // `crossing` partway, and went on from beat 40 by what was left.
                    const double without = before + kBlock * rate;
                    REQUIRE(without >= crossing);  // not before the grid point...
                    REQUIRE(before < crossing);    // ... and in the block that reached it
                    const double expected = grid.sampleAt(40.0) + (without - crossing);
                    CHECK(std::abs(now - expected) <= 1.0);
                    // The phase: the same beat fraction as without the jump.
                    const double phase = grid.beatAt(now) - std::floor(grid.beatAt(now));
                    const double phaseWithout = grid.beatAt(without) - std::floor(grid.beatAt(without));
                    CHECK(std::abs(phase - phaseWithout) * spb <= 1.0);
                }
                else
                {
                    CHECK(rig.deck(0).queuedActions == 1);
                }
                before = now;
            }
            CHECK(jumped);
            CHECK(rig.deck(0).queuedActions == 0);
        }
}

TEST_CASE("Quantize: off, nothing waits; pause calls a waiting play off; the resolution", "[engine][quantize]")
{
    const auto track = silentTrack(30.0);
    const auto grid = util::Beatgrid::constant(0.5 * kRate, 128.0, kRate);
    Rig rig;
    rig.engine.send(engine::LoadTrack{0, track.get(), 1});
    rig.engine.send(engine::SetBeatgrid{0, grid});
    rig.engine.send(engine::LoadTrack{1, track.get(), 2});
    rig.engine.send(engine::SetBeatgrid{1, grid});
    rig.engine.send(engine::Play{0});
    rig.blocks(10);
    // Off: play is immediate.
    rig.engine.send(engine::Play{1});
    rig.block();
    CHECK(rig.deck(1).playing);
    rig.engine.send(engine::Pause{1});
    // On: it waits; pause (or play / pause again) calls it off.
    rig.engine.send(engine::SetQuantize{true, 1.0f});
    rig.block();
    rig.engine.send(engine::TogglePlay{1});
    rig.block();
    if (!rig.deck(1).playing) // (unless the beat happened to fall in that block)
    {
        CHECK(rig.deck(1).queuedActions == 1);
        rig.engine.send(engine::TogglePlay{1});
        rig.block();
        CHECK(rig.deck(1).queuedActions == 0);
        rig.blocks(60);
        CHECK_FALSE(rig.deck(1).playing);
    }
    // Nothing leads the clock: play is immediate even with quantize on.
    {
        Rig alone;
        alone.engine.send(engine::LoadTrack{0, track.get(), 1});
        alone.engine.send(engine::SetBeatgrid{0, grid});
        alone.engine.send(engine::SetQuantize{true, 1.0f});
        alone.engine.send(engine::Play{0});
        alone.block();
        CHECK(alone.deck(0).playing);
    }
    // The resolution snaps to 1/8, 1/4, 1/2 or 1.
    rig.engine.send(engine::SetQuantize{true, 0.3f});
    rig.block();
    CHECK(rig.engine.readState().quantize);
    CHECK(rig.engine.readState().quantizeBeats == 0.25f);
    rig.engine.send(engine::SetQuantize{false, 7.0f});
    rig.block();
    CHECK_FALSE(rig.engine.readState().quantize);
    CHECK(rig.engine.readState().quantizeBeats == 1.0f);
}
