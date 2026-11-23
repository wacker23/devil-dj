#pragma once

// The Beat Maker's piano roll (P4-07, BEATMAKER_SPEC §6) for one synth track of a pattern:
// - Toolbar: synth tracks 1-4 (a new one on first use), its sound (every factory synth preset), snap (1/4 .. 1/32,
//   triplets), scale (root + scale, highlighted rows, optional snapping), chord stamp, QUANTIZE, the arpeggiator
//   (on, rate, mode, octaves).
// - Grid: pitch rows (scroll with the wheel), the pattern's ticks across. Click an empty place to draw a note (or
//   the chord), dragging on draws it longer; drag a note to move it, its right edge to resize it; right-click
//   deletes. The velocity lane under the grid: drag across the notes.
// - Keyboard (the Beat Maker keys): arrows move the cursor, Enter adds / removes the note (chord) at it, Ctrl+arrows
//   move the note under the cursor, Alt+Left / Right make it shorter / longer, Shift+Up / Down its velocity, and the
//   toolbar on Q / C / K / Shift+K / G / A / Shift+A / Alt+A / Ctrl+A.

#include "beatmaker/Model.h"
#include "ui/Actions.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <optional>
#include <vector>

namespace devildj::ui
{

class PianoRoll final : public juce::Component, public juce::SettableTooltipClient
{
public:
    PianoRoll();

    void setProject(beatmaker::Project* project);
    void setPattern(int pattern);
    void setTrack(int synthTrack); // 0..3 (created when edited)
    void modelChanged();           // the project changed underneath (undo): redraw, keep the cursor in range

    std::function<void()> onChanged;
    bool isDragging() const noexcept { return drag != Drag::None; }

    bool perform(GlobalAction action);

    // Gestures (public for tests).
    void pressAt(juce::Point<int> p, juce::ModifierKeys mods);
    void dragTo(juce::Point<int> p);
    void release();
    void scrollRows(int rows);

    // Geometry and state.
    juce::Point<int> pointFor(int tick, int pitch) const; // the top-left of that tick / pitch
    int rowHeight() const noexcept { return 12; }
    int lowestPitch() const noexcept { return lowPitch; }
    int visiblePitches() const noexcept;
    juce::Rectangle<int> velocityLane() const noexcept { return laneArea; }
    juce::Rectangle<int> noteArea() const noexcept { return gridArea; }
    int track() const noexcept { return trackIndex; }
    int cursorTick() const noexcept { return cursorAt; }
    int cursorPitch() const noexcept { return cursorNote; }
    int snap() const noexcept;
    int scale() const noexcept { return scaleIndex; }
    int root() const noexcept { return rootNote; }
    bool scaleSnapping() const noexcept { return scaleSnap; }
    int chord() const noexcept { return chordIndex; }
    beatmaker::SynthTrack* current();
    beatmaker::SynthTrack& ensureCurrent() { return ensureTrack(); }

    juce::ComboBox& soundBox() { return sound; }
    juce::ComboBox& scaleBox() { return scaleChoice; }
    juce::ComboBox& rootBox() { return rootChoice; }
    juce::ComboBox& chordBox() { return chordChoice; }
    juce::TextButton& trackButton(int i) { return trackButtons[static_cast<std::size_t>(i)]; }
    juce::TextButton& quantizeButton() { return quantizeNow; }
    juce::TextButton& arpButton() { return arpOn; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

private:
    beatmaker::Pattern* pattern();
    beatmaker::SynthTrack& ensureTrack();
    void edited();
    void refresh();
    int tickAt(int x) const;
    int pitchAt(int y) const;
    int placePitch(int pitch) const; // through the scale if snapping
    void stamp(int tick, int pitch);  // a note or the chord at (tick, pitch), snap long
    std::optional<std::size_t> cursorNoteIndex();
    void keepCursorVisible();
    void setSoundChoice(int choice);

    beatmaker::Project* project = nullptr;
    int patternIndex = 0, trackIndex = 0;
    int lowPitch = 48;
    int cursorAt = 0, cursorNote = 60;
    int division = 16;
    bool triplet = false;
    int scaleIndex = 0, rootNote = 0, chordIndex = 0;
    bool scaleSnap = false;

    enum class Drag
    {
        None,
        Draw,
        Move,
        Resize,
        Lane
    } drag = Drag::None;
    std::vector<std::size_t> dragNotes; // the notes a drag moves (a chord drawn moves together)
    std::vector<beatmaker::Note> dragStart;
    int dragTick = 0, dragPitch = 0;

    std::array<juce::TextButton, beatmaker::kMaxSynthTracks> trackButtons;
    juce::ComboBox sound, scaleChoice, rootChoice, chordChoice;
    juce::TextButton snapButton{"1/16"}, scaleSnapButton{"SNAP TO SCALE"}, quantizeNow{"QUANTIZE"};
    juce::TextButton arpOn{"ARP"}, arpRate{"1/16"}, arpMode{"UP"}, arpOctaves{"1 OCT"};
    juce::Rectangle<int> gridArea, laneArea, keysArea;
};

} // namespace devildj::ui
