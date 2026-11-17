#pragma once

#include "engine/AudioDeviceService.h"

#include <juce_audio_utils/juce_audio_utils.h>

namespace devildj::app
{

// Temporary audio settings page (P1-01): JUCE's device selector (type, device, sample rate, buffer size)
// plus a live status line with latency, xruns and CPU load. Replaced by the full page in P1-19.
class AudioSettingsComponent final : public juce::Component, private juce::Timer
{
public:
    explicit AudioSettingsComponent(engine::AudioDeviceService& audio);

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override;

    engine::AudioDeviceService& audio;
    juce::AudioDeviceSelectorComponent selector;
    juce::Label status;
};

// Opens the settings page as a non-modal dialog window owned by JUCE (closes itself).
void showAudioSettingsWindow(engine::AudioDeviceService& audio, juce::Component* parent);

} // namespace devildj::app
