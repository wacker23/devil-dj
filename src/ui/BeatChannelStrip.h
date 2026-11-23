#pragma once

// The Beat Maker's mixer channel "BEATS" (P4-10) beside its panel: level, filter (the channel's color FX knob),
// CUE, the crossfader side (A / THRU / B), and the tempo - following the master or its own BPM.
// Reads the engine state for the channel; the tempo is the project's (onTempo edits it).

#include "engine/AudioEngine.h"
#include "ui/MixerControls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace devildj::ui
{

class BeatChannelStrip final : public juce::Component
{
public:
    explicit BeatChannelStrip(engine::AudioEngine& engine);

    std::function<void(bool follow, double ownBpm)> onTempo;
    void setTempo(bool follow, double ownBpm); // from the project
    void refresh();                            // from the engine state (the app's timer)

    juce::TextButton& cueButton() { return cue; }
    juce::TextButton& assignButton() { return assign; }
    juce::TextButton& tempoButton() { return tempo; }
    ValueControl& levelFader() { return *level; }
    ValueControl& bpmKnob() { return *bpm; }

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void sendParam(engine::ChannelParam p, float value);

    engine::AudioEngine& engine;
    juce::TextButton cue{"CUE"}, assign{"THRU"}, tempo{"FOLLOW"};
    std::unique_ptr<ValueControl> level, filter, bpm;
    bool follow = true;
    double ownBpm = 120.0;
};

} // namespace devildj::ui
