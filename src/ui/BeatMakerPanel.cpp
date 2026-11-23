#include "ui/BeatMakerPanel.h"

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kText(0xffe8e8ee);
const juce::Colour kAccent(0xffe0483c);

// One edit: the whole project before and after (a project is a few kilobytes).
class Snapshot final : public juce::UndoableAction
{
public:
    Snapshot(beatmaker::Project& target, beatmaker::Project before, beatmaker::Project after)
        : project(target)
        , was(std::move(before))
        , now(std::move(after))
    {
    }
    bool perform() override
    {
        project = now;
        return true;
    }
    bool undo() override
    {
        project = was;
        return true;
    }
    int getSizeInUnits() override { return 1; }

private:
    beatmaker::Project& project;
    beatmaker::Project was, now;
};
} // namespace

BeatMakerPanel::BeatMakerPanel()
{
    for (auto* b : {&drumsTab, &synthTab})
    {
        b->setColour(juce::TextButton::buttonColourId, kPanel);
        b->setColour(juce::TextButton::buttonOnColourId, kAccent);
        b->setColour(juce::TextButton::textColourOffId, kText);
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(*b);
    }
    drumsTab.setTooltip("The drum tracks' step grid (Tab switches)");
    synthTab.setTooltip("The synth tracks' piano roll (Tab switches)");
    drumsTab.onClick = [this] { showView(0); };
    synthTab.onClick = [this] { showView(1); };
    addAndMakeVisible(stepGrid);
    addChildComponent(pianoRoll);
    stepGrid.onChanged = [this] { record(); };
    pianoRoll.onChanged = [this] { record(); };
    stepGrid.onPattern = [this](int p) { pianoRoll.setPattern(p); };
    stepGrid.onPlayStop = [this]
    {
        if (onPlayStop)
            onPlayStop();
    };
    showView(0);
}

void BeatMakerPanel::setProject(beatmaker::Project* p)
{
    project = p;
    stepGrid.setProject(p);
    pianoRoll.setProject(p);
    pianoRoll.setPattern(stepGrid.pattern());
    projectReplaced();
}

void BeatMakerPanel::projectReplaced()
{
    undo.clearUndoHistory();
    if (project != nullptr)
        snapshot = *project;
    dragging = false;
}

void BeatMakerPanel::record()
{
    if (project == nullptr || *project == snapshot)
        return;
    // A drag is one step: a new transaction when it starts, the rest merged into it.
    const bool drag = stepGrid.isDragging() || pianoRoll.isDragging();
    if (!drag || !dragging)
        undo.beginNewTransaction();
    dragging = drag;
    undo.perform(new Snapshot(*project, snapshot, *project));
    snapshot = *project;
    if (onChanged)
        onChanged();
}

void BeatMakerPanel::afterHistory()
{
    if (project != nullptr)
        snapshot = *project;
    dragging = false;
    stepGrid.modelChanged();
    pianoRoll.modelChanged();
    if (onChanged)
        onChanged();
}

bool BeatMakerPanel::undoLast()
{
    if (!undo.undo())
        return false;
    afterHistory();
    return true;
}

bool BeatMakerPanel::redoLast()
{
    if (!undo.redo())
        return false;
    afterHistory();
    return true;
}

void BeatMakerPanel::showView(int v)
{
    shownView = v == 1 ? 1 : 0;
    stepGrid.setVisible(shownView == 0);
    pianoRoll.setVisible(shownView == 1);
    drumsTab.setToggleState(shownView == 0, juce::dontSendNotification);
    synthTab.setToggleState(shownView == 1, juce::dontSendNotification);
    if (isShowing())
        (shownView == 0 ? static_cast<juce::Component&>(stepGrid) : pianoRoll).grabKeyboardFocus();
}

bool BeatMakerPanel::perform(GlobalAction a)
{
    using GA = GlobalAction;
    switch (a)
    {
        case GA::BeatMakerUndo:
            undoLast();
            return true;
        case GA::BeatMakerRedo:
            redoLast();
            return true;
        case GA::BeatMakerView:
            showView(1 - shownView);
            return true;
        default:
            break;
    }
    // A key press is its own undo step even if a drag is somehow still marked.
    dragging = false;
    if (shownView == 0)
        return stepGrid.perform(a);
    // Pattern keys and start / stop work the same in both views (through the grid, which owns the pattern).
    const int ai = static_cast<int>(a);
    if ((ai >= static_cast<int>(GA::BeatMakerPattern1) && ai <= static_cast<int>(GA::BeatMakerPattern8)) ||
        a == GA::BeatMakerStartStop || a == GA::BeatMakerLength || a == GA::BeatMakerSwingDown || a == GA::BeatMakerSwingUp)
        return stepGrid.perform(a);
    if (a == GA::BeatMakerCopy || a == GA::BeatMakerPaste)
    {
        auto* t = pianoRoll.current();
        if (a == GA::BeatMakerCopy && t != nullptr)
            noteClipboard = t->notes;
        else if (a == GA::BeatMakerPaste && !noteClipboard.empty() && project != nullptr)
        {
            pianoRoll.ensureCurrent().notes = noteClipboard;
            record();
            pianoRoll.modelChanged();
        }
        return true;
    }
    return pianoRoll.perform(a);
}

void BeatMakerPanel::resized()
{
    auto area = getLocalBounds();
    auto tabs = area.removeFromLeft(64).reduced(2);
    drumsTab.setBounds(tabs.removeFromTop(40).reduced(1));
    synthTab.setBounds(tabs.removeFromTop(40).reduced(1));
    stepGrid.setBounds(area);
    pianoRoll.setBounds(area);
}

void BeatMakerPanel::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
}

} // namespace devildj::ui
