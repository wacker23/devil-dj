// P4-11: live record. Done when: manual test (these cover the parts underneath).

#include "app/BeatMakerProducer.h"
#include "beatmaker/DrumKits.h"
#include "beatmaker/Sequencer.h"
#include "engine/Scheduler.h"
#include "ui/PadModes.h"
#include "ui/StepGrid.h"

#include <catch2/catch_test_macros.hpp>

using namespace devildj;
using beatmaker::Sequencer;

TEST_CASE("Live record: a hit turns the nearest step on (overdub, nothing turned off)", "[beatmaker][record]")
{
    auto p = beatmaker::defaultProject();
    p.patterns[0].drums = beatmaker::kitTracks(beatmaker::drumKits()[0]);
    p.patterns[0].drums[1].steps[4].on = true;
    CHECK(beatmaker::recordHit(p, {0, 6}, 1, 90));
    CHECK(p.patterns[0].drums[1].steps[6].on);
    CHECK(p.patterns[0].drums[1].steps[6].velocity == 90);
    CHECK(p.patterns[0].drums[1].steps[4].on); // the old one stays
    CHECK(beatmaker::recordHit(p, {0, 6}, 1, 200));
    CHECK(p.patterns[0].drums[1].steps[6].velocity == 127);
    CHECK_FALSE(beatmaker::recordHit(p, {0, 16}, 1, 100)); // no step 16 in a 16-step pattern
    CHECK_FALSE(beatmaker::recordHit(p, {0, 2}, 9, 100));  // no track 9
    CHECK_FALSE(beatmaker::recordHit(p, {-1, 2}, 0, 100)); // not playing
    // What is recorded plays on the next pass.
    Sequencer seq;
    seq.playPattern(0, 0);
    std::vector<beatmaker::BeatEvent> out;
    seq.emit(p, 4, 8, out); // the second pass
    bool found = false;
    for (const auto& e : out)
        found = found || (e.track == 1 && e.step == 6);
    CHECK(found);
}

TEST_CASE("Live record: a live hit plays at once, never counted late", "[beatmaker][record]")
{
    engine::EventBuffer<8> buffer;
    engine::ScheduledEvent e;
    e.sampleTime = engine::kNow;
    buffer.insert(e);
    int offset = -1;
    buffer.takeDue(100000, 256, [&](const engine::ScheduledEvent&, int o) { offset = o; });
    CHECK(offset == 0);
    CHECK(buffer.lateEvents() == 0);
}

TEST_CASE("Live record: the producer sounds the hit and finds the nearest step", "[beatmaker][record]")
{
    std::vector<engine::ScheduledEvent> pushed;
    app::BeatMakerProducer producer([&](const engine::ScheduledEvent& e) { pushed.push_back(e); return true; }, false);
    double seconds = 0.0;
    producer.now = [&] { return seconds; };
    auto p = beatmaker::defaultProject();
    p.patterns[0].drums = beatmaker::kitTracks(beatmaker::drumKits()[0]);
    p.patterns[2].drums = beatmaker::kitTracks(beatmaker::drumKits()[1]);
    producer.setProject(std::make_shared<const beatmaker::Project>(p));
    producer.setClock({0, 0.0, 120.0, 48000.0, 0.0});
    // Stopped: heard (the shown pattern's sound), no step.
    CHECK_FALSE(producer.hit(2, 3, 100));
    REQUIRE(pushed.size() == 1);
    CHECK(pushed[0].sampleTime == engine::kNow);
    CHECK(pushed[0].soundIndex == 2 * 16 + 3);
    CHECK(pushed[0].kind == engine::VoiceKind::DrumSynth);
    CHECK(pushed[0].chokeGroup == 1); // the closed hat
    // Playing pattern A from beat 0 at 120 BPM: 0.26 s = beat 0.52 = step 2.08 -> 2; 0.32 s = step 2.56 -> 3.
    producer.play(0, false);
    seconds = 0.26;
    auto at = producer.hit(2, 0, 100);
    REQUIRE(at);
    CHECK(at->pattern == 0);
    CHECK(at->step == 2);
    CHECK(pushed.back().soundIndex == 0); // the pattern playing, not the one shown
    seconds = 0.32;
    CHECK(producer.hit(2, 0, 100)->step == 3);
    seconds = 1.99; // just before the bar line: the next pass's step 0
    CHECK(producer.hit(0, 0, 100)->step == 0);
}

TEST_CASE("Live record: REC on the grid, 1-8 and the track names hit, the BEAT PADS mode", "[beatmaker][record][ui]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto project = beatmaker::defaultProject();
    project.patterns[0].drums = beatmaker::kitTracks(beatmaker::drumKits()[0]);
    ui::StepGrid grid;
    grid.setBounds(0, 0, 1400, 420);
    grid.setProject(&project);
    using GA = ui::GlobalAction;
    std::vector<int> hits;
    bool recording = false;
    grid.onHit = [&](int t) { hits.push_back(t); };
    grid.onRecord = [&](bool on) { recording = on; };
    grid.perform(GA::BeatMakerTrack3);
    CHECK(hits.empty()); // not recording: 1-8 only select
    grid.perform(GA::BeatMakerRecord);
    CHECK(recording);
    CHECK(grid.recording());
    grid.perform(GA::BeatMakerTrack3);
    grid.perform(GA::BeatMakerTrack1);
    CHECK(hits == std::vector<int>{2, 0});
    grid.recordButton().onClick();
    CHECK_FALSE(recording);
    // A click on track 5's name hits it (recording or not).
    hits.clear();
    const auto row5 = grid.cellBounds(4, 0);
    grid.pressAt({grid.soundBox(4).getX() - 20, row5.getCentreY()}, {});
    CHECK(hits == std::vector<int>{4});
    CHECK(grid.cursorTrack() == 4);
    CHECK(juce::String(ui::padModeName(ui::PadMode::BeatPads)) == "BEAT PADS");
    CHECK(ui::kPadModeCount == 12); // P6-05 added Drum Swap
}
