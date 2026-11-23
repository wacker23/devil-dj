#pragma once

// The Beat Maker's bottom panel (P4-07): DRUMS (the step grid) or SYNTH (the piano roll) for the pattern shown,
// Tab switching between them, and the edit history - every edit a snapshot of the project in a juce::UndoManager,
// one undo step per key press or click, a whole drag as one (Ctrl+Z / Ctrl+Y). P4-08: drag snapshots coalesce;
// at least 100 steps are kept, and older ones beyond a 64 MB history are dropped.

#include "beatmaker/Model.h"
#include "ui/Actions.h"
#include "ui/PianoRoll.h"
#include "ui/StepGrid.h"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace devildj::ui
{

class BeatMakerPanel final : public juce::Component
{
public:
    BeatMakerPanel();

    void setProject(beatmaker::Project* project);
    std::function<void()> onChanged; // any edit, undo or redo
    std::function<void()> onPlayStop;

    bool perform(GlobalAction action);
    void showView(int view); // 0 drums, 1 synth
    int view() const noexcept { return shownView; }

    StepGrid& grid() noexcept { return stepGrid; }
    PianoRoll& roll() noexcept { return pianoRoll; }
    juce::UndoManager& history() noexcept { return undo; }
    bool undoLast();
    bool redoLast();
    // The project was replaced from outside (a file loaded): history starts again.
    void projectReplaced();

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void record();       // after an edit: one snapshot step (drags merge)
    void afterHistory(); // after undo / redo: the views follow

    beatmaker::Project* project = nullptr;
    beatmaker::Project snapshot;
    bool dragging = false;
    int lastGesture = -1; // grid presses + roll presses when the last step began
    int shownView = 0;
    std::vector<beatmaker::Note> noteClipboard;
    // At least 100 steps kept; beyond that, older ones go once the history holds more than 64 MB.
    static constexpr int kHistoryBytes = 64 << 20;
    static constexpr int kMinimumSteps = 100;
    juce::UndoManager undo{kHistoryBytes, kMinimumSteps};
    juce::TextButton drumsTab{"DRUMS"}, synthTab{"SYNTH"};
    StepGrid stepGrid;
    PianoRoll pianoRoll;
};

} // namespace devildj::ui
