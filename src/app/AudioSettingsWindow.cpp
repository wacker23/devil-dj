#include "AudioSettingsWindow.h"

namespace devildj::app
{

namespace
{
constexpr int kStatusHeight = 48;
}

AudioSettingsComponent::AudioSettingsComponent(engine::AudioDeviceService& audioService)
    : audio(audioService)
    , selector(audioService.getDeviceManager(),
               0,                                              // min inputs
               0,                                              // max inputs (no mic yet)
               engine::AudioDeviceService::kNumOutputChannels, // min outputs
               engine::AudioDeviceService::kNumOutputChannels, // max outputs
               false,                                          // MIDI inputs
               false,                                          // MIDI output
               true,                                           // channels as stereo pairs
               false)                                          // advanced options visible
{
    addAndMakeVisible(selector);

    status.setJustificationType(juce::Justification::centredLeft);
    status.setFont(juce::FontOptions(14.0f));
    addAndMakeVisible(status);

    setSize(520, 380);
    timerCallback();
    startTimerHz(4);
}

void AudioSettingsComponent::resized()
{
    auto area = getLocalBounds().reduced(8);
    status.setBounds(area.removeFromBottom(kStatusHeight));
    selector.setBounds(area);
}

void AudioSettingsComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void AudioSettingsComponent::timerCallback()
{
    const auto s = audio.getStatus();
    if (!s.isOpen)
    {
        status.setText("No audio device open" + (s.lastError.isEmpty() ? juce::String() : ": " + s.lastError),
                       juce::dontSendNotification);
        return;
    }

    status.setText(juce::String::formatted("Latency %.1f ms (buffer %.1f ms + driver %.1f ms)\n"
                                           "Xruns: %d    Audio CPU: %.0f %%",
                                           s.totalOutputLatencyMs, s.bufferMs, s.outputLatencyMs, s.xruns,
                                           100.0 * s.cpuLoad),
                   juce::dontSendNotification);
}

void showAudioSettingsWindow(engine::AudioDeviceService& audio, juce::Component* parent)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new AudioSettingsComponent(audio)); // JUCE takes ownership
    options.dialogTitle = "Audio settings";
    options.componentToCentreAround = parent;
    options.dialogBackgroundColour =
        juce::LookAndFeel::getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
}

} // namespace devildj::app
