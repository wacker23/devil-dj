#pragma once

// The transition editor (P4-17): the outgoing track's end above the incoming track's start, both on one beat grid
// (transition beats; 2 bars before it, 1 after), the lanes drawn over them - the outgoing deck's over the top
// half, the incoming deck's over the bottom - the chosen lane bold with its points.
//
// Mouse: click a lane point to pick it and drag it (beats snap to quarter beats; Alt: free), click elsewhere in a
// half to add a point there, right-click a point to delete it, double-click it to change its shape; Shift-drag a
// half to move that track against the other (the incoming by beats, the outgoing by bars).
// Keyboard (the Transition key context): 1-6 the lane, Tab the deck, Left / Right the cursor (a beat; Shift a
// bar), Ctrl+Left / Right the previous / next point, Up / Down its value (a point added if there is none), Enter
// adds a point, Delete removes it, S its shape, L the length, P / Shift+P the presets, A Auto, , / . the incoming
// track a beat earlier / later (Shift: the outgoing one a bar), Space preview, Ctrl+S save as a preset.

#include "analysis/WaveformBuilder.h"
#include "engine/TransitionCurves.h"
#include "ui/Actions.h"
#include "util/Beatgrid.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace devildj::ui
{

class TransitionEditor final : public juce::Component, public juce::SettableTooltipClient
{
public:
    struct Track
    {
        const analysis::Waveform* waveform = nullptr; // owned by the track buffer (kept while shown)
        util::Beatgrid grid;
        int numSamples = 0;
        juce::String title;
    };
    struct Edit
    {
        engine::TransitionCurves curves;
        double outBeat = 0.0; // the outgoing track's grid beat where it starts
        double inBeat = 0.0;  // the incoming track's grid beat it starts from
        juce::String preset;  // "Auto", a preset's name, or "Custom" once edited
        std::vector<std::string> reasons;
    };

    TransitionEditor();

    void setTracks(const Track& outgoing, const Track& incoming);
    void setEdit(const Edit& edit); // shown and edited from here on
    const Edit& edit() const noexcept { return current; }
    void setPresetNames(const juce::StringArray& builtIn, const juce::StringArray& user);
    void setMessage(const juce::String& text); // when there is nothing to edit (no two tracks)

    std::function<void(const Edit&)> onChanged;   // any edit
    std::function<void()> onAuto;                 // AUTO: the app plans it again
    std::function<void(int index)> onPreset;      // a built-in preset (0..) or a user one (built-ins + i)
    std::function<void(const Edit&)> onPreview;   // play it from 2 bars before
    std::function<void(const juce::String& name)> onSavePreset;

    bool perform(GlobalAction action);

    // Gestures (public for tests): a point in this component's coordinates.
    void pressAt(juce::Point<int> p, juce::ModifierKeys mods);
    void dragTo(juce::Point<int> p, juce::ModifierKeys mods);
    void release();

    // Geometry: x <-> transition beats; a lane value <-> y in its half.
    int xOf(double beat) const noexcept;
    double beatAt(int x) const noexcept;
    int yOf(bool incoming, engine::Lane lane, float value) const noexcept;
    float valueAt(bool incoming, engine::Lane lane, int y) const noexcept;
    juce::Rectangle<int> half(bool incoming) const noexcept;

    engine::Lane lane() const noexcept { return selectedLane; }
    bool incomingSide() const noexcept { return incoming; }
    double cursor() const noexcept { return cursorBeat; }
    int bars() const noexcept;
    static const std::vector<int>& lengths(); // the bar lengths offered: 1, 2, 4, 8, 12, 16, 24, 32

    juce::TextButton& laneButton(int i) { return laneButtons[static_cast<std::size_t>(i)]; }
    juce::ComboBox& lengthBox() { return length; }
    juce::ComboBox& presetBox() { return presets; }
    juce::TextButton& autoButton() { return autoNow; }
    juce::TextButton& previewButton() { return preview; }
    juce::TextButton& fxButton() { return fxType; }
    juce::TextButton& loopButton() { return loop; }
    static const std::vector<int>& fxChoices(); // the effects a transition can send into (-1: none)

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

private:
    engine::LaneCurve& laneCurve(bool side, engine::Lane l) { return current.curves.lane(side, l); }
    int pointNear(bool side, engine::Lane l, juce::Point<int> p) const; // index or -1
    int pointAtCursor() const;                                          // on the chosen lane, or -1
    void changed(bool custom = true);
    void refresh();
    void setLength(int bars);
    void drawWaveform(juce::Graphics& g, const Track& t, double startBeat, juce::Rectangle<int> area) const;
    static float step(engine::Lane l);
    static void rangeOf(engine::Lane l, float& lo, float& hi);

    Track outTrack, inTrack;
    Edit current;
    juce::String message = "Load a track on each deck, one of them playing: the transition between them is edited here.";
    engine::Lane selectedLane = engine::Lane::Volume;
    bool incoming = false;
    double cursorBeat = 0.0;
    // Drag state.
    enum class Drag
    {
        None,
        Point,
        MoveTrack
    } drag = Drag::None;
    int dragIndex = -1;
    bool dragSide = false;
    int dragStartX = 0;
    double dragStartBeat = 0.0;
    int builtInCount = 0;

    std::array<juce::TextButton, engine::kLaneCount> laneButtons;
    juce::ComboBox length, presets;
    juce::TextButton autoNow{"AUTO"}, preview{"PREVIEW"}, save{"SAVE PRESET"};
    juce::TextButton fxType{"FX: NONE"}, fxTime{"3/4"}, loop{"NO LOOP"};
    juce::Rectangle<int> view, reasonsArea;
};

} // namespace devildj::ui
