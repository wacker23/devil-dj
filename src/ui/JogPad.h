#pragma once

#include "engine/AudioEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace devildj::ui
{

// Temporary jog strip for one deck (P1-09) until the zoomed waveform (P1-14) takes over the same gestures:
//   drag left/right   scratch while playing (release eases back to the tempo), scrub while paused
//   mouse wheel       fine nudge (about 5 ms per notch)
// Grab-and-move like the waveform: dragging left pushes the track forward. 1 px = 5 ms of track.
// Works with mouse and touch (touch arrives as mouse events).
class JogPad final : public juce::Component
{
public:
    JogPad(engine::AudioEngine& engine, engine::DeckId deck);

    static constexpr double kSecondsPerPixel = 0.005;
    static constexpr double kNudgeSecondsPerNotch = 0.005;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

private:
    double samplesPerSecond() const;
    static double uiClockSeconds();

    engine::AudioEngine& engine;
    engine::DeckId deck;
    float lastX = 0.0f;
    bool held = false;
    juce::VBlankAttachment vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(JogPad)
};

} // namespace devildj::ui
