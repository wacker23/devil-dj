// P4-08: undo / redo across Beat Maker edits. Done when: 100-step undo test.

#include "beatmaker/DrumKits.h"
#include "ui/BeatMakerPanel.h"

#include <catch2/catch_test_macros.hpp>

#include <random>
#include <vector>

using namespace devildj;
using GA = ui::GlobalAction;

TEST_CASE("Beat Maker undo: 100 mixed edits undone one by one back to the start, then all redone",
          "[ui][undo]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto project = beatmaker::defaultProject();
    project.patterns[0].drums = beatmaker::kitTracks(beatmaker::drumKits()[0]);
    ui::BeatMakerPanel panel;
    panel.setBounds(0, 0, 1400, 480);
    panel.setProject(&project);
    std::vector<beatmaker::Project> states{project};
    std::mt19937 rng(8);
    const GA gridEdits[] = {GA::BeatMakerToggleStep, GA::BeatMakerVelocityUp, GA::BeatMakerProbabilityDown,
                            GA::BeatMakerMicroLater, GA::BeatMakerRatchet,    GA::BeatMakerMute,
                            GA::BeatMakerSoundNext,  GA::BeatMakerSwingUp,    GA::BeatMakerLength};
    const GA rollEdits[] = {GA::BeatMakerToggleStep, GA::BeatMakerMicroLater, GA::BeatMakerVelocityDown,
                            GA::BeatMakerNoteUp,     GA::BeatMakerArp,        GA::BeatMakerArpMode};
    int edits = 0;
    while (edits < 100)
    {
        const int kind = std::uniform_int_distribution<int>(0, 9)(rng);
        if (kind < 5)
        {
            panel.showView(0);
            panel.perform(GA::BeatMakerCursorRight); // moves only: no undo step
            panel.perform(gridEdits[std::uniform_int_distribution<int>(0, 8)(rng)]);
        }
        else if (kind < 9)
        {
            panel.showView(1);
            panel.perform(GA::BeatMakerCursorRight);
            panel.perform(rollEdits[std::uniform_int_distribution<int>(0, 5)(rng)]);
        }
        else
        {
            // A drag on the grid: one step however many cells it paints.
            panel.showView(0);
            auto& g = panel.grid();
            const int row = std::uniform_int_distribution<int>(0, 7)(rng);
            g.pressAt(g.cellBounds(row, 0).getCentre(), {});
            for (int s = 1; s < 8; ++s)
                g.dragTo(g.cellBounds(row, s).getCentre(), {});
            g.release();
            CHECK(panel.history().getNumActionsInCurrentTransaction() == 1); // coalesced into one snapshot
        }
        if (!(project == states.back()))
        {
            states.push_back(project);
            ++edits;
        }
    }
    // Back, one step at a time, through every state.
    for (int i = 100; i > 0; --i)
    {
        REQUIRE(panel.undoLast());
        INFO("after undo to state " << i - 1);
        CHECK(project == states[static_cast<std::size_t>(i - 1)]);
    }
    CHECK_FALSE(panel.undoLast());
    CHECK(project == states.front());
    // And forward again.
    for (int i = 1; i <= 100; ++i)
    {
        REQUIRE(panel.redoLast());
        CHECK(project == states[static_cast<std::size_t>(i)]);
    }
    CHECK_FALSE(panel.redoLast());
    // The keys do the same.
    panel.perform(GA::BeatMakerUndo);
    CHECK(project == states[99]);
    panel.perform(GA::BeatMakerRedo);
    CHECK(project == states[100]);
}

TEST_CASE("Beat Maker undo: at least 100 steps kept, a loaded project starts a new history", "[ui][undo]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto project = beatmaker::defaultProject();
    ui::BeatMakerPanel panel;
    panel.setBounds(0, 0, 1400, 480);
    panel.setProject(&project);
    for (int i = 0; i < 150; ++i)
    {
        panel.perform(GA::BeatMakerCursorRight);
        panel.perform(GA::BeatMakerToggleStep);
    }
    int undone = 0;
    while (panel.undoLast())
        ++undone;
    CHECK(undone >= 100);
    CHECK(undone == 150); // a small project: all of them fit the budget
    // Replacing the project (a file loaded) clears the history.
    panel.redoLast();
    project = beatmaker::defaultProject();
    panel.projectReplaced();
    CHECK_FALSE(panel.undoLast());
    CHECK_FALSE(panel.redoLast());
}

TEST_CASE("Beat Maker undo: a new pattern's rows aren't an undo step of their own", "[ui][undo]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto project = beatmaker::defaultProject();
    project.patterns[0].drums = beatmaker::kitTracks(beatmaker::drumKits()[0]);
    ui::BeatMakerPanel panel;
    panel.setBounds(0, 0, 1400, 480);
    panel.setProject(&project);
    panel.perform(GA::BeatMakerToggleStep); // A, step 1
    panel.perform(GA::BeatMakerPattern3);   // C gets its rows
    REQUIRE(project.patterns[2].drums.size() == 8);
    const auto shown = project;
    panel.perform(GA::BeatMakerToggleStep); // C, step 1
    panel.perform(GA::BeatMakerUndo);
    CHECK(project == shown); // C's rows still there, its step off
    panel.perform(GA::BeatMakerUndo);
    CHECK_FALSE(project.patterns[0].drums[0].steps[0].on);
}
