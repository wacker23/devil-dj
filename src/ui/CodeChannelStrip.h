#pragma once

// The code deck's mixer channel "CODE" (P5-12): level, filter (the channel's color FX knob), CUE, the crossfader
// side (A / THRU / B) and a status line (playing, voices, late events, the last warning or error). Reads the
// engine state for the channel; the status text comes from the app (the code deck's producer).

#include "engine/AudioEngine.h"
#include "ui/MixerControls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace devildj::ui
{

class CodeChannelStrip final : public juce::Component
{
public:
    explicit CodeChannelStrip(engine::AudioEngine& engine);

    void refresh();                          // from the engine state (the app's timer)
    void setStatus(const juce::String& text); // the producer's state, one short line

    juce::TextButton& cueButton() { return cue; }
    juce::TextButton& assignButton() { return assign; }
    ValueControl& levelFader() { return *level; }
    ValueControl& filterKnob() { return *filter; }
    const juce::String& status() const noexcept { return statusText; }

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void sendParam(engine::ChannelParam p, float value);

    engine::AudioEngine& engine;
    juce::TextButton cue{"CUE"}, assign{"THRU"};
    std::unique_ptr<ValueControl> level, filter;
    juce::String statusText;
};

} // namespace devildj::ui
