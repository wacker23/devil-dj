// P4-07: the piano roll. Done when: manual test; undo works.

#include "beatmaker/DrumKits.h"
#include "beatmaker/NoteEdit.h"
#include "ui/BeatMakerPanel.h"
#include "ui/PianoRoll.h"

#include <catch2/catch_test_macros.hpp>

using namespace devildj;
using beatmaker::Note;
using GA = ui::GlobalAction;

TEST_CASE("Note editing: scales, chords, snap, quantize", "[beatmaker][notes]")
{
    using namespace beatmaker;
    const int major = 1, minorPent = 7;
    CHECK(inScale(64, 0, major));       // E in C major
    CHECK_FALSE(inScale(63, 0, major)); // Eb
    CHECK(snapToScale(63, 0, major) == 62);       // ties go down
    CHECK(snapToScale(61, 0, minorPent) == 60);
    CHECK(stepInScale(60, 0, major, 1) == 62);
    CHECK(stepInScale(60, 0, major, -1) == 59);
    CHECK(stepInScale(60, 9, minorPent, 1) == 62); // A minor pentatonic: C -> D
    CHECK(snapToScale(61, 0, 0) == 61);            // chromatic: untouched
    CHECK(chordPitches(60, 1) == std::vector<int>{60, 64, 67});
    CHECK(chordPitches(57, 5) == std::vector<int>{57, 60, 64, 67}); // Am7
    CHECK(chordPitches(125, 1) == std::vector<int>{125});           // out of range dropped
    CHECK(snapTicks(16, false) == 24);
    CHECK(snapTicks(4, false) == 96);
    CHECK(snapTicks(8, true) == 32); // 1/8 triplet
    CHECK(snapNearest(35, 24) == 24);
    CHECK(snapNearest(37, 24) == 48);
    SynthTrack t;
    t.notes = {{50, 10, 60, 100}, {3, 30, 64, 100}, {49, 40, 60, 90}, {100, 24, 67, 100}};
    quantize(t, 24);
    REQUIRE(t.notes.size() == 3); // the two near 48 on pitch 60 merged (the longer kept)
    CHECK(t.notes[0].startTick == 0);
    CHECK(t.notes[1].startTick == 48);
    CHECK(t.notes[1].lengthTicks == 40);
    CHECK(t.notes[2].startTick == 96);
    CHECK(noteAt(t, 60, 60) == std::optional<std::size_t>(1));
    CHECK_FALSE(noteAt(t, 60, 61));
}

namespace
{
struct Fixture
{
    juce::ScopedJuceInitialiser_GUI gui;
    beatmaker::Project project = beatmaker::defaultProject();
    ui::BeatMakerPanel panel;
    int changes = 0;
    Fixture()
    {
        project.patterns[0].synths.clear();
        panel.setBounds(0, 0, 1400, 480);
        panel.setProject(&project);
        panel.onChanged = [this] { ++changes; };
        panel.showView(1);
    }
    ui::PianoRoll& roll() { return panel.roll(); }
    std::vector<Note>& notes(int track = 0) { return project.patterns[0].synths[static_cast<std::size_t>(track)].notes; }
    juce::Point<int> at(int tick, int pitch) // the middle of that tick's snap cell on that row
    {
        const auto p = roll().pointFor(tick, pitch);
        return {p.x + 3, p.y + roll().rowHeight() / 2};
    }
};
} // namespace

TEST_CASE("Piano roll by mouse: draw, draw longer, move, resize, delete, chord stamp, scale snap, velocity lane",
          "[ui][pianoroll]")
{
    Fixture f;
    auto& r = f.roll();
    const int low = r.lowestPitch();
    // Click draws a 1/16 note; dragging on draws it longer.
    r.pressAt(f.at(0, low + 5), {});
    r.dragTo(f.at(70, low + 5));
    r.release();
    REQUIRE(f.notes().size() == 1);
    CHECK(f.notes()[0].pitch == low + 5);
    CHECK(f.notes()[0].startTick == 0);
    CHECK(f.notes()[0].lengthTicks == 72);
    // Move it: 2 snaps later, 3 rows up.
    r.pressAt(f.at(10, low + 5), {});
    r.dragTo(f.at(58, low + 8));
    r.release();
    CHECK(f.notes()[0].startTick == 48);
    CHECK(f.notes()[0].pitch == low + 8);
    // Resize from its right edge.
    const auto& n = f.notes()[0];
    const auto edge = r.pointFor(n.startTick + n.lengthTicks, n.pitch);
    r.pressAt({edge.x - 2, edge.y + 5}, {});
    r.dragTo(f.at(200, low + 8));
    r.release();
    CHECK(f.notes()[0].startTick == 48);
    CHECK(f.notes()[0].lengthTicks == 168);
    // A chord stamp: a minor chord in one click.
    r.chordBox().setSelectedId(3, juce::sendNotificationSync); // Minor
    r.pressAt(f.at(240, low + 2), {});
    r.release();
    CHECK(f.notes().size() == 4);
    // Scale snap (C major): a click on a black key's row lands in the scale.
    r.chordBox().setSelectedId(1, juce::sendNotificationSync);
    r.scaleBox().setSelectedId(2, juce::sendNotificationSync); // Major
    r.perform(GA::BeatMakerScaleSnap);
    int black = low;
    while (beatmaker::inScale(black, 0, 1))
        ++black;
    r.pressAt(f.at(300, black), {});
    r.release();
    bool landed = false;
    for (const auto& note : f.notes())
        landed = landed || (note.startTick == 288 && beatmaker::inScale(note.pitch, 0, 1));
    CHECK(landed);
    // Right-click deletes.
    const auto count = f.notes().size();
    r.pressAt(f.at(60, low + 8), juce::ModifierKeys::rightButtonModifier);
    r.release();
    CHECK(f.notes().size() == count - 1);
    // The velocity lane over the chord's start: its three notes.
    const auto lane = r.velocityLane();
    r.pressAt({r.pointFor(244, 0).x, lane.getY() + 2}, {});
    r.release();
    int loud = 0;
    for (const auto& note : f.notes())
        loud += note.startTick == 240 && note.velocity > 120;
    CHECK(loud == 3);
    // Wheel scroll moves the rows.
    r.scrollRows(5);
    CHECK(r.lowestPitch() == low + 5);
    juce::Image image(juce::Image::ARGB, 1400, 480, true);
    juce::Graphics g(image);
    f.panel.paintEntireComponent(g, false);
}

TEST_CASE("Piano roll by keyboard: cursor, add / remove, move, length, velocity, toolbar, tracks, copy / paste",
          "[ui][pianoroll]")
{
    Fixture f;
    auto& p = f.panel;
    auto& r = f.roll();
    const int start = r.cursorPitch();
    p.perform(GA::BeatMakerToggleStep);
    REQUIRE(f.notes().size() == 1);
    CHECK(f.notes()[0].pitch == start);
    p.perform(GA::BeatMakerMicroLater); // longer
    CHECK(f.notes()[0].lengthTicks == 48);
    p.perform(GA::BeatMakerVelocityDown);
    CHECK(f.notes()[0].velocity == 92);
    p.perform(GA::BeatMakerNoteRight);
    p.perform(GA::BeatMakerNoteUp);
    CHECK(f.notes()[0].startTick == 24);
    CHECK(f.notes()[0].pitch == start + 1);
    CHECK(r.cursorTick() == 24); // the cursor goes with it
    p.perform(GA::BeatMakerToggleStep); // removes the note under the cursor
    CHECK(f.notes().empty());
    // Cursor moves by the snap; the snap cycles 1/16 -> 1/32.
    p.perform(GA::BeatMakerCursorRight);
    CHECK(r.cursorTick() == 48);
    p.perform(GA::BeatMakerSnap);
    CHECK(r.snap() == 12);
    p.perform(GA::BeatMakerCursorUp);
    CHECK(r.cursorPitch() == start + 2);
    // Chord, scale, quantize, arp on the keys.
    p.perform(GA::BeatMakerChord);
    CHECK(r.chord() == 1);
    p.perform(GA::BeatMakerToggleStep);
    CHECK(f.notes().size() == 3);
    p.perform(GA::BeatMakerScale);
    CHECK(r.scale() == 1);
    f.notes()[0].startTick = 50;
    p.perform(GA::BeatMakerSnap); // 1/32 -> 1/4 triplet
    p.perform(GA::BeatMakerSnap); // -> 1/8 triplet (32 ticks)
    p.perform(GA::BeatMakerQuantize);
    for (const auto& n : f.notes())
        CHECK(n.startTick % 32 == 0);
    p.perform(GA::BeatMakerArp);
    p.perform(GA::BeatMakerArpRate);
    p.perform(GA::BeatMakerArpMode);
    p.perform(GA::BeatMakerArpOctaves);
    const auto& arp = f.project.patterns[0].synths[0].arp;
    CHECK(arp.on);
    CHECK(arp.rate == 32);
    CHECK(arp.mode == beatmaker::ArpSettings::Mode::Down);
    CHECK(arp.octaves == 2);
    // Sound: next preset.
    const auto before = f.project.patterns[0].synths[0].preset.name;
    p.perform(GA::BeatMakerSoundNext);
    CHECK(f.project.patterns[0].synths[0].preset.name != before);
    // Copy track 1's notes to track 3 (made on the way).
    p.perform(GA::BeatMakerCopy);
    p.perform(GA::BeatMakerTrack3);
    CHECK(r.track() == 2);
    p.perform(GA::BeatMakerPaste);
    REQUIRE(f.project.patterns[0].synths.size() == 3);
    CHECK(f.notes(2) == f.notes(0));
    p.perform(GA::BeatMakerClear);
    CHECK(f.notes(2).empty());
    // Tab: back to the drums; pattern keys work in both views.
    p.perform(GA::BeatMakerView);
    CHECK(p.view() == 0);
    p.perform(GA::BeatMakerView);
    p.perform(GA::BeatMakerPattern2);
    CHECK(p.grid().pattern() == 1);
}

TEST_CASE("Beat Maker undo: each edit one step, a drag one step, across the grid and the roll; redo; new edits "
          "drop the redo",
          "[ui][pianoroll][undo]")
{
    Fixture f;
    auto& p = f.panel;
    auto& r = f.roll();
    const auto empty = f.project;
    // Roll: a drawn note dragged longer is one step.
    r.pressAt(f.at(0, r.lowestPitch() + 3), {});
    r.dragTo(f.at(50, r.lowestPitch() + 3));
    r.dragTo(f.at(90, r.lowestPitch() + 3));
    r.release();
    const auto drawn = f.project;
    // Grid: two toggles, two steps.
    p.showView(0);
    p.perform(GA::BeatMakerToggleStep);
    p.perform(GA::BeatMakerCursorRight);
    p.perform(GA::BeatMakerToggleStep);
    CHECK(f.project.patterns[0].drums[0].steps[1].on);
    p.perform(GA::BeatMakerUndo);
    CHECK_FALSE(f.project.patterns[0].drums[0].steps[1].on);
    CHECK(f.project.patterns[0].drums[0].steps[0].on);
    p.perform(GA::BeatMakerUndo);
    CHECK(f.project == drawn);
    p.perform(GA::BeatMakerUndo); // the whole drawn note at once
    CHECK(f.project == empty);
    CHECK_FALSE(p.undoLast()); // nothing more
    p.perform(GA::BeatMakerRedo);
    CHECK(f.project == drawn);
    p.perform(GA::BeatMakerRedo);
    CHECK(f.project.patterns[0].drums[0].steps[0].on);
    // A new edit drops what could be redone.
    p.perform(GA::BeatMakerSwingUp);
    CHECK_FALSE(p.redoLast());
    p.perform(GA::BeatMakerUndo);
    CHECK(f.project.swing == 0.0f);
    // The views follow an undo: the roll's track is still there to draw on.
    p.showView(1);
    CHECK(f.roll().current() != nullptr);
    CHECK(f.changes > 5);
}
