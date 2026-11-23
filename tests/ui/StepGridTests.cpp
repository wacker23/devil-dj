// P4-06: the step grid. Done when: everything editable by keyboard and mouse.

#include "beatmaker/DrumKits.h"
#include "ui/Actions.h"
#include "ui/KeyboardController.h"
#include "ui/Keymaps.h"
#include "ui/StepGrid.h"

#include <catch2/catch_test_macros.hpp>

using namespace devildj;
using GA = ui::GlobalAction;

namespace
{
beatmaker::Project kitProject()
{
    auto p = beatmaker::defaultProject();
    p.patterns[0].drums = beatmaker::kitTracks(beatmaker::drumKits()[0]);
    return p;
}

struct Fixture
{
    juce::ScopedJuceInitialiser_GUI gui;
    beatmaker::Project project = kitProject();
    ui::StepGrid grid;
    int changes = 0;
    Fixture()
    {
        grid.setBounds(0, 0, 1200, 420);
        grid.setProject(&project);
        grid.onChanged = [this] { ++changes; };
    }
    beatmaker::Step& at(int track, int step, int pattern = 0)
    {
        return project.patterns[static_cast<std::size_t>(pattern)].drums[static_cast<std::size_t>(track)].steps[static_cast<std::size_t>(step)];
    }
};
} // namespace

TEST_CASE("Step grid by keyboard: cursor, toggle, velocity, probability, micro-timing, ratchet, track, sound, "
          "mute / solo, length, page, swing, pattern, copy / paste / clear",
          "[ui][stepgrid]")
{
    Fixture f;
    auto& g = f.grid;
    CHECK(g.perform(GA::BeatMakerCursorRight));
    CHECK(g.perform(GA::BeatMakerCursorRight));
    CHECK(g.perform(GA::BeatMakerCursorDown));
    CHECK(g.cursorTrack() == 1);
    CHECK(g.cursorStep() == 2);
    g.perform(GA::BeatMakerToggleStep);
    CHECK(f.at(1, 2).on);
    g.perform(GA::BeatMakerVelocityDown);
    CHECK(f.at(1, 2).velocity == 92);
    g.perform(GA::BeatMakerVelocityUp);
    g.perform(GA::BeatMakerVelocityUp);
    CHECK(f.at(1, 2).velocity == 108);
    g.perform(GA::BeatMakerProbabilityDown);
    CHECK(f.at(1, 2).probability == 90);
    g.perform(GA::BeatMakerMicroLater);
    g.perform(GA::BeatMakerMicroLater);
    CHECK(f.at(1, 2).microTiming == 20);
    for (int i = 0; i < 9; ++i)
        g.perform(GA::BeatMakerMicroEarlier);
    CHECK(f.at(1, 2).microTiming == -50); // held at the end of its range
    g.perform(GA::BeatMakerRatchet);
    CHECK(f.at(1, 2).ratchet == 2);
    // Wrap-around: left from step 0 goes to the last; up from track 0 to the last.
    g.perform(GA::BeatMakerTrack1);
    CHECK(g.cursorTrack() == 0);
    g.perform(GA::BeatMakerTrack8);
    CHECK(g.cursorTrack() == 7);
    g.perform(GA::BeatMakerCursorDown);
    CHECK(g.cursorTrack() == 0);
    // Sound, mute, solo on the cursor track.
    const auto before = f.project.patterns[0].drums[0].sound;
    g.perform(GA::BeatMakerSoundNext);
    CHECK_FALSE(f.project.patterns[0].drums[0].sound == before);
    g.perform(GA::BeatMakerSoundPrevious);
    CHECK(f.project.patterns[0].drums[0].sound == before);
    g.perform(GA::BeatMakerMute);
    g.perform(GA::BeatMakerSolo);
    CHECK(f.project.patterns[0].drums[0].mute);
    CHECK(f.project.patterns[0].drums[0].solo);
    // Length 16 -> 32, the second page, swing.
    g.perform(GA::BeatMakerLength);
    CHECK(f.project.patterns[0].lengthSteps == 32);
    CHECK(f.project.patterns[0].drums[5].steps.size() == 32);
    CHECK(g.pages() == 2);
    g.perform(GA::BeatMakerPageNext);
    CHECK(g.page() == 1);
    CHECK(g.cursorStep() >= 16);
    g.perform(GA::BeatMakerPagePrevious);
    CHECK(g.page() == 0);
    g.perform(GA::BeatMakerSwingUp);
    g.perform(GA::BeatMakerSwingUp);
    CHECK(std::abs(f.project.swing - 0.1f) < 1e-6f);
    g.perform(GA::BeatMakerSwingDown);
    CHECK(std::abs(f.project.swing - 0.05f) < 1e-6f);
    // Copy track 2's steps, paste onto track 3, clear track 2.
    g.perform(GA::BeatMakerTrack2);
    g.perform(GA::BeatMakerCopy);
    g.perform(GA::BeatMakerTrack3);
    g.perform(GA::BeatMakerPaste);
    CHECK(f.at(2, 2).on);
    CHECK(f.at(2, 2).ratchet == 2);
    g.perform(GA::BeatMakerTrack2);
    g.perform(GA::BeatMakerClear);
    CHECK_FALSE(f.at(1, 2).on);
    // Pattern C: its own steps, the same sounds as A.
    int shownPattern = -1;
    g.onPattern = [&](int p) { shownPattern = p; };
    g.perform(GA::BeatMakerPattern3);
    CHECK(g.pattern() == 2);
    CHECK(shownPattern == 2);
    CHECK(f.project.patterns[2].drums.size() == 8);
    CHECK(f.project.patterns[2].drums[0].sound == f.project.patterns[0].drums[0].sound);
    g.perform(GA::BeatMakerToggleStep);
    CHECK(f.project.patterns[2].drums[1].steps[2].on);
    CHECK_FALSE(f.at(1, 2).on); // A untouched
    // Space: start / stop.
    int playStops = 0;
    g.onPlayStop = [&] { ++playStops; };
    g.perform(GA::BeatMakerStartStop);
    CHECK(playStops == 1);
    CHECK(f.changes > 15);
    CHECK_FALSE(g.perform(GA::LoadSuggestion)); // not a Beat Maker action
}

TEST_CASE("Step grid by mouse: click toggles, drag paints, Alt-drag velocity, the lane, headers, pattern bar",
          "[ui][stepgrid]")
{
    Fixture f;
    auto& g = f.grid;
    const auto centre = [&](int t, int s) { return g.cellBounds(t, s).getCentre(); };
    // Click toggles; a drag along the row paints the same state.
    g.pressAt(centre(0, 0), {});
    CHECK(f.at(0, 0).on);
    g.dragTo(centre(0, 1), {});
    g.dragTo(centre(0, 3), {});
    g.dragTo(centre(4, 5), {}); // off the row: still paints the row it started on
    g.release();
    CHECK(f.at(0, 1).on);
    CHECK(f.at(0, 3).on);
    CHECK(f.at(0, 5).on);
    CHECK_FALSE(f.at(4, 5).on);
    // Starting on a lit step paints it off.
    g.pressAt(centre(0, 1), {});
    g.dragTo(centre(0, 3), {});
    g.release();
    CHECK_FALSE(f.at(0, 1).on);
    CHECK_FALSE(f.at(0, 3).on);
    CHECK(f.at(0, 0).on);
    // Alt-drag: the height in the cell sets the velocity (top = loud).
    const auto cell = g.cellBounds(2, 4);
    g.pressAt({cell.getCentreX(), cell.getY() + 1}, juce::ModifierKeys::altModifier);
    CHECK(f.at(2, 4).on);
    CHECK(f.at(2, 4).velocity > 115);
    g.dragTo({cell.getCentreX(), cell.getBottom() - 2}, juce::ModifierKeys::altModifier);
    CHECK(f.at(2, 4).velocity < 20);
    g.release();
    // The velocity lane of the cursor track (now track 2).
    CHECK(g.cursorTrack() == 2);
    const auto lane = g.velocityLane();
    const int colW = lane.getWidth() / 16;
    g.pressAt({lane.getX() + 4 * colW + colW / 2, lane.getCentreY()}, {});
    CHECK(std::abs(static_cast<int>(f.at(2, 4).velocity) - 64) < 6);
    g.release();
    // Outside the grid: nothing.
    CHECK_FALSE(g.cellAt({2, 2}).has_value());
    // Headers: sound, mute, solo.
    const auto& choices = ui::StepGrid::soundChoices();
    REQUIRE(choices.size() > 30);
    g.soundBox(5).setSelectedId(3, juce::sendNotificationSync);
    CHECK(f.project.patterns[0].drums[5].sound.synth == choices[2].synth);
    g.muteButton(6).onClick();
    g.soloButton(7).onClick();
    CHECK(f.project.patterns[0].drums[6].mute);
    CHECK(f.project.patterns[0].drums[7].solo);
    // Pattern bar: pattern B, the length button, the swing knob.
    g.patternButton(1).onClick();
    CHECK(g.pattern() == 1);
    g.lengthButton().onClick();
    CHECK(f.project.patterns[1].lengthSteps == 32);
    g.swingKnob().dragBy(60.0f, false);
    CHECK(f.project.swing > 0.1f);
    // The step editor: right-click opens it (headless here: it only needs not to crash), Shift+Enter too.
    g.pressAt(centre(0, 0), juce::ModifierKeys::rightButtonModifier);
    CHECK(g.perform(GA::BeatMakerStepEditor));
    // Painting with the second page shown.
    g.perform(GA::BeatMakerPageNext);
    g.pressAt(centre(1, 0), {});
    CHECK(f.project.patterns[1].drums[1].steps[16].on);
    juce::Image image(juce::Image::ARGB, 1200, 420, true);
    juce::Graphics gr(image);
    g.setPlayback(true, 17);
    g.paintEntireComponent(gr, false);
}

TEST_CASE("Step grid keys: every Beat Maker action has a key in the Beat Maker context, none clash",
          "[ui][stepgrid][keys]")
{
    const auto map = ui::defaultKeymap();
    for (int a = static_cast<int>(GA::BeatMakerStartStop); a <= static_cast<int>(GA::BeatMakerLoadSample); ++a)
    {
        const auto action = static_cast<GA>(a);
        if (action == GA::BeatMakerUndo || action == GA::BeatMakerRedo)
            continue; // P4-08
        INFO(ui::commandKey(ui::globalCommand(action)));
        CHECK_FALSE(ui::keysOf(map, ui::globalCommand(action), ui::KeyContext::BeatMaker).empty());
    }
    CHECK(ui::findConflicts(map).empty());
    ui::ActionDispatcher d;
    ui::KeyboardController k(d);
    CHECK(k.lookup({ui::scan::Equals, ui::mods::None}, ui::KeyContext::BeatMaker) == ui::globalCommand(GA::BeatMakerSwingUp));
    CHECK(k.lookup({ui::scan::Tab, ui::mods::Ctrl}, ui::KeyContext::BeatMaker) == ui::globalCommand(GA::SwitchPanel));
}
