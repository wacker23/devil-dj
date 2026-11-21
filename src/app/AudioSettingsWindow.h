#pragma once

#include "engine/AudioDeviceService.h"
#include "engine/AudioEngine.h"
#include "engine/CueDevice.h"
#include "engine/OutputRouter.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace devildj::app
{

// Audio settings page (P1-19), laid out as the three things to do when setting up a laptop and headphones:
//   1 Sound card  - driver, output device, sample rate, buffer size (with the delay each buffer gives)
//   2 Headphones  - output layout: speakers only / split cable (left ear master, right ear cue) / 4-channel
//   3 Check       - test tone in the speakers and in the headphones, latency, glitch counter, CPU
// Changes apply immediately. Done when: the owner can set up the laptop + headphones without help.
class AudioSettingsComponent final : public juce::Component, private juce::Timer
{
public:
    struct Callbacks
    {
        std::function<engine::OutputLayout()> getLayout;
        std::function<void(engine::OutputLayout)> setLayout; // saves the setting, tells engine + device
        // P2-25: the separate headphone device (may be null: no such option).
        engine::CueDevice* cueDevice = nullptr;
        std::function<juce::String(const juce::String&)> setCueDevice; // saves it, opens it; error or empty
    };

    AudioSettingsComponent(engine::AudioDeviceService& audio, engine::AudioEngine& engine, Callbacks callbacks);
    ~AudioSettingsComponent() override;

    void resized() override;
    void paint(juce::Graphics& g) override;

    // Text helpers (public for tests).
    static juce::String bufferText(int samples, double sampleRate); // "256 samples  (5.3 ms)"
    enum class LatencyRating
    {
        Great, // <= 12 ms: fine for scratching
        Good,  // <= 25 ms: fine for mixing
        High,  // more: noticeable; try a smaller buffer or another driver
    };
    static LatencyRating rateLatency(double totalMs) noexcept;
    static juce::String layoutTooltip();

    // Controls, exposed for tests.
    juce::ComboBox& driverBox() noexcept { return driver; }
    juce::ComboBox& deviceBox() noexcept { return device; }
    juce::ComboBox& rateBox() noexcept { return rate; }
    juce::ComboBox& bufferBox() noexcept { return buffer; }
    juce::TextButton& layoutButton(engine::OutputLayout l) noexcept
    {
        return layoutButtons[static_cast<std::size_t>(l)];
    }
    juce::TextButton& toneSpeakersButton() noexcept { return toneSpeakers; }
    juce::TextButton& toneHeadphonesButton() noexcept { return toneHeadphones; }
    juce::TextButton& toneStopButton() noexcept { return toneStop; }
    juce::TextButton& resetGlitchesButton() noexcept { return resetGlitches; }
    juce::ComboBox& cueDeviceBox() noexcept { return cueDeviceList; }
    int glitchesSinceReset() const noexcept;
    void refreshFromDevice(); // re-reads the device lists and choices (timer does this when they change)

private:
    void timerCallback() override;
    void playTone(engine::Bus bus);
    void stopTone();
    void showError(const juce::String& error);
    void selectLayout(engine::OutputLayout layout);

    engine::AudioDeviceService& audio;
    engine::AudioEngine& engine;
    Callbacks callbacks;

    juce::TooltipWindow tooltips{this, 400};
    juce::ComboBox driver, device, rate, buffer;
    juce::ComboBox cueDeviceList; // P2-25: the headphone device for the separate-device layout
    juce::Label bufferHint, errorLabel;
    std::array<juce::TextButton, engine::kNumOutputLayouts> layoutButtons;
    juce::TextButton toneSpeakers{"Tone in speakers"}, toneHeadphones{"Tone in headphones"}, toneStop{"Stop tone"};
    juce::TextButton resetGlitches{"Reset"};
    bool toneOn = false;
    int glitchBaseline = 0;
    juce::String shownSetup; // what the combos show, to notice outside changes
    engine::AudioDeviceStatus status;

    juce::Rectangle<int> section1, section2, section3, readouts;
    std::array<juce::Rectangle<int>, 4> rowLabels; // Driver, Output, Sample rate, Buffer
};

// Opens the settings page as a non-modal dialog window owned by JUCE (closes itself).
void showAudioSettingsWindow(engine::AudioDeviceService& audio, engine::AudioEngine& engine,
                             AudioSettingsComponent::Callbacks callbacks, juce::Component* parent);

} // namespace devildj::app
