#pragma once

#include "engine/AudioDeviceService.h"
#include "engine/AudioEngine.h"
#include "engine/OutputRouter.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <functional>

namespace devildj::app
{

// Temporary audio settings page (P1-01, P1-03): output layout, JUCE's device selector (type, device, sample
// rate, buffer size), test tones for master and cue, and a live status line with latency, xruns, CPU and
// routing. Replaced by the full page in P1-19.
class AudioSettingsComponent final : public juce::Component, private juce::Timer
{
public:
    struct Callbacks
    {
        std::function<engine::OutputLayout()> getLayout;
        std::function<void(engine::OutputLayout)> setLayout; // saves the setting, tells engine + device
    };

    AudioSettingsComponent(engine::AudioDeviceService& audio, engine::AudioEngine& engine, Callbacks callbacks);
    ~AudioSettingsComponent() override;

    void resized() override;
    void paint(juce::Graphics& g) override;

    static juce::String layoutTooltip();

private:
    void timerCallback() override;
    void playTone(engine::Bus bus);
    void stopTone();

    engine::AudioDeviceService& audio;
    engine::AudioEngine& engine;
    Callbacks callbacks;

    juce::TooltipWindow tooltips{this, 400};
    juce::Label layoutLabel;
    juce::ComboBox layoutBox;
    juce::AudioDeviceSelectorComponent selector;
    juce::Label toneLabel;
    juce::TextButton toneMaster{"Tone: master"};
    juce::TextButton toneCue{"Tone: cue"};
    juce::TextButton toneStop{"Stop"};
    juce::Label status;
    bool toneOn = false;
};

// Opens the settings page as a non-modal dialog window owned by JUCE (closes itself).
void showAudioSettingsWindow(engine::AudioDeviceService& audio, engine::AudioEngine& engine,
                             AudioSettingsComponent::Callbacks callbacks, juce::Component* parent);

} // namespace devildj::app
