// P3-07: slicer. Done when: manual test on a 128 BPM track sounds on-grid. These check the grid side in the
// engine: the domain on the bar, its shift, the roll starting exactly on slice boundaries, the return exactly to
// where the track would be.

#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "ui/PadModes.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace devildj;

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
    // 128 BPM: a beat every 22500 samples, beat 0 at 0.5 s.
    util::Beatgrid grid = util::Beatgrid::constant(0.5 * kRate, 128.0, kRate);
    Rig()
    {
        track.sampleRate = kRate;
        track.audio.setSize(2, static_cast<int>(90.0 * kRate));
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
    const engine::DeckState& deck() { return engine.readState().decks[0]; }
    double beatOf(double sample) const { return grid.beatAt(sample); }
    double sampleOf(double beat) const { return grid.sampleAt(beat); }
};
} // namespace

TEST_CASE("Slicer: the domain starts on the bar and shifts on as the track plays", "[engine][slicer]")
{
    Rig rig;
    rig.engine.send(engine::Seek{0, rig.sampleOf(10.3)});
    rig.engine.send(engine::Play{0});
    rig.engine.send(engine::SetSlicer{0, true, 8.0});
    rig.block();
    REQUIRE(rig.deck().slicerOn);
    CHECK(rig.deck().slicerStart == rig.sampleOf(8.0)); // the bar at or before beat 10.3
    CHECK(rig.deck().slicerEnd == rig.sampleOf(16.0));
    CHECK(rig.deck().slicerCurrent == 2);                // beat 10.x: the third 1-beat slice
    // Play on past beat 16: the next 8 beats.
    while (rig.deck().positionSamples < rig.sampleOf(16.2))
        rig.block();
    CHECK(rig.deck().slicerStart == rig.sampleOf(16.0));
    CHECK(rig.deck().slicerCurrent == 0);
    // 16 beats: 2-beat slices from the bar.
    rig.engine.send(engine::SetSlicer{0, true, 16.0});
    rig.block();
    CHECK(rig.deck().slicerEnd - rig.deck().slicerStart == rig.sampleOf(32.0) - rig.sampleOf(16.0));
    // Off.
    rig.engine.send(engine::SetSlicer{0, false});
    rig.block();
    CHECK_FALSE(rig.deck().slicerOn);
}

TEST_CASE("Slicer: a held pad rolls its slice from the next boundary; letting go returns exactly", "[engine][slicer]")
{
    for (const bool slip : {false, true})
    {
        INFO("slip " << slip);
        Rig rig;
        rig.engine.send(engine::SetSlip{0, slip});
        rig.engine.send(engine::Seek{0, rig.sampleOf(8.0)});
        rig.engine.send(engine::Play{0});
        rig.engine.send(engine::SetSlicer{0, true, 8.0});
        rig.block();
        const double started = rig.sampleOf(8.0);
        int played = 1;
        while (rig.deck().positionSamples < rig.sampleOf(9.3)) // into slice 2 (beat 9.x)
        {
            rig.block();
            ++played;
        }
        rig.engine.send(engine::SlicerPad{0, 5}); // slice 6: beats 13..14
        rig.block();
        ++played;
        CHECK_FALSE(rig.deck().loopActive); // waits for the next slice boundary (beat 10)
        CHECK(rig.deck().slipping);
        // Follow it block by block: once past beat 10 on the timeline it rolls 13..14, on the grid.
        double lowest = 1e18, highest = 0.0;
        for (int b = 0; b < 300; ++b) // ~1.6 s, 3+ beats
        {
            rig.block();
            ++played;
            if (rig.deck().loopActive)
            {
                lowest = std::min(lowest, rig.deck().positionSamples);
                highest = std::max(highest, rig.deck().positionSamples);
            }
        }
        REQUIRE(rig.deck().loopActive);
        CHECK(rig.deck().loopStart == rig.sampleOf(13.0));
        CHECK(rig.deck().loopEnd == rig.sampleOf(14.0));
        CHECK(lowest >= rig.sampleOf(13.0));
        CHECK(highest <= rig.sampleOf(14.0));
        CHECK(rig.deck().slicerRolling == 5);
        // The roll is in phase with the timeline: the playhead's place in the beat = the track's.
        const double phase = std::fmod(rig.beatOf(rig.deck().positionSamples), 1.0);
        const double timelinePhase = std::fmod(rig.beatOf(rig.deck().slipPositionSamples), 1.0);
        CHECK(std::abs(phase - timelinePhase) < 1e-6);
        // Let go: where the track would be, exactly.
        rig.engine.send(engine::SlicerPad{0, 5, true});
        rig.block();
        ++played;
        CHECK_FALSE(rig.deck().loopActive);
        CHECK_FALSE(rig.deck().slipping);
        CHECK(rig.deck().slicerRolling == -1);
        CHECK(std::abs(rig.deck().positionSamples - (started + played * static_cast<double>(kBlock))) < 1e-6);
    }
}

TEST_CASE("Slicer: two pads - the last pressed rolls; up again, the other one; the pads light", "[engine][slicer]")
{
    Rig rig;
    rig.engine.send(engine::Seek{0, rig.sampleOf(8.0)});
    rig.engine.send(engine::Play{0});
    rig.engine.send(engine::SetSlicer{0, true, 8.0});
    rig.block();
    CHECK(ui::padLook(ui::PadMode::Slicer, 0, rig.deck(), false).lit); // the track is in slice 1
    rig.engine.send(engine::SlicerPad{0, 1});
    rig.blocks(100);
    rig.engine.send(engine::SlicerPad{0, 6});
    rig.blocks(100);
    CHECK(rig.deck().slicerRolling == 6);
    CHECK(rig.deck().loopStart == rig.sampleOf(14.0));
    CHECK(ui::padLook(ui::PadMode::Slicer, 6, rig.deck(), false).lit);
    CHECK_FALSE(ui::padLook(ui::PadMode::Slicer, 0, rig.deck(), false).lit);
    rig.engine.send(engine::SlicerPad{0, 6, true});
    rig.blocks(100);
    CHECK(rig.deck().slicerRolling == 1);
    CHECK(rig.deck().loopStart == rig.sampleOf(9.0));
    CHECK(rig.deck().slipping);
    rig.engine.send(engine::SlicerPad{0, 1, true});
    rig.block();
    CHECK_FALSE(rig.deck().slipping);
    CHECK_FALSE(rig.deck().loopActive);
    // Pad commands: Shift + pad 2 = a 16-beat domain; paused, a pad does nothing.
    const auto cmds = ui::padCommands(ui::PadMode::Slicer, 0, 1, 0, true, false, rig.deck());
    REQUIRE(cmds.size() == 1);
    CHECK(std::get<engine::SetSlicer>(cmds[0]).beats == 16.0);
    rig.engine.send(engine::Pause{0});
    rig.engine.send(engine::SlicerPad{0, 3});
    rig.block();
    CHECK(rig.deck().slicerRolling == -1);
}
