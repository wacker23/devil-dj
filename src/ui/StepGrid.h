#pragma once

// The Beat Maker's step grid (P4-06, BEATMAKER_SPEC §6): the pattern bar (play / stop, patterns A-H, length,
// page, swing), a header per drum track (name, sound, mute / solo), the grid (16 steps a page, bar and beat
// shading, the cursor, the playing step) and the cursor track's velocity lane. Mouse: click toggles a step, drag
// paints, Alt-drag sets velocity, right-click opens the step's probability / micro-timing / ratchet editor, drag
// in the velocity lane sets velocities. Every edit also has a key (the Beat Maker key context), through perform().
//
// It edits the Project it is given (message thread) and reports each edit through onChanged.

#include "beatmaker/Model.h"
#include "ui/Actions.h"
#include "ui/MixerControls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace devildj::ui
{

class StepGrid final : public juce::Component, public juce::SettableTooltipClient
{
public:
    StepGrid();
    ~StepGrid() override;

    void setProject(beatmaker::Project* project); // not owned
    beatmaker::Project* getProject() const noexcept { return project; }

    std::function<void()> onChanged;          // any edit to the project
    std::function<void()> onPlayStop;         // the PLAY button / Space
    std::function<void(int pattern)> onPattern; // the pattern shown changed (A-H)
    bool isDragging() const noexcept { return drag != Drag::None; }
    void modelChanged(); // the project changed underneath (undo): keep the cursor in range, redraw

    // The playing step of the shown pattern (-1: stopped), and whether the Beat Maker plays (the button's light).
    void setPlayback(bool playing, int step);

    // Keyboard: a Beat Maker action (false if it isn't one this grid handles).
    bool perform(GlobalAction action);

    // The gestures the mouse handlers use (public for tests). Points are in this component's coordinates.
    void pressAt(juce::Point<int> p, juce::ModifierKeys mods);
    void dragTo(juce::Point<int> p, juce::ModifierKeys mods);
    void release();

    struct Cell
    {
        int track = 0, step = 0;
    };
    std::optional<Cell> cellAt(juce::Point<int> p) const; // a grid cell (its absolute step on the shown page)
    juce::Rectangle<int> cellBounds(int track, int step) const;
    juce::Rectangle<int> velocityLane() const noexcept { return laneArea; }

    int pattern() const noexcept { return patternIndex; }
    int cursorTrack() const noexcept { return track; }
    int cursorStep() const noexcept { return step; }
    int page() const noexcept { return pageIndex; }
    int pages() const noexcept;
    beatmaker::Pattern& shown();

    // Sound choices: every drum synth preset ("Kick: Tight", ...).
    struct SoundChoice
    {
        juce::String label;
        std::string synth, preset;
    };
    static const std::vector<SoundChoice>& soundChoices();
    juce::ComboBox& soundBox(int row) { return rows[static_cast<std::size_t>(row)].sound; }
    juce::TextButton& muteButton(int row) { return rows[static_cast<std::size_t>(row)].mute; }
    juce::TextButton& soloButton(int row) { return rows[static_cast<std::size_t>(row)].solo; }
    juce::TextButton& patternButton(int i) { return patternButtons[static_cast<std::size_t>(i)]; }
    juce::TextButton& lengthButton() { return length; }
    ValueControl& swingKnob() { return swing; }

    // The step editor (right-click): its three values for the step at (track, step).
    void openStepEditor(int track, int step);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    struct Row
    {
        juce::Label name;
        juce::ComboBox sound;
        juce::TextButton mute{"M"}, solo{"S"};
    };

    beatmaker::DrumTrack* trackAt(int index);
    beatmaker::Step* stepAt(int trackIndex, int stepIndex);
    void edited(bool layoutToo = false);
    void refresh(); // controls <- the model
    void selectPattern(int index);
    void moveCursor(int dTrack, int dStep);
    void setSound(int trackIndex, int choice);
    void ensureTracks();

    beatmaker::Project* project = nullptr;
    int patternIndex = 0, track = 0, step = 0, pageIndex = 0;
    bool playing = false;
    int playingStep = -1;
    std::vector<beatmaker::Step> clipboard;

    // Drag state.
public:
    enum class Drag
    {
        None,
        Paint,
        Velocity,
        Lane
    };

private:
    Drag drag = Drag::None;
    bool paintOn = true;
    int dragTrack = 0;

    juce::TextButton play{"PLAY"}, length{"16 STEPS"}, pageButton{"PAGE 1/1"};
    std::array<juce::TextButton, beatmaker::kPatternCount> patternButtons;
    ValueControl swing{"SWING", ValueControl::Style::Knob, 0.0};
    std::array<Row, beatmaker::kMaxDrumTracks> rows;
    juce::Rectangle<int> gridArea, laneArea, headerArea;
    static constexpr int kVisibleSteps = 16;
};

} // namespace devildj::ui
