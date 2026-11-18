#include "AudioSettingsWindow.h"

namespace devildj::app
{

namespace
{
constexpr int kRowHeight = 28;
constexpr int kLabelWidth = 150;
constexpr int kStatusHeight = 64;
constexpr float kToneGainDb = -18.0f;
} // namespace

juce::String AudioSettingsComponent::layoutTooltip()
{
    return "How the master mix and your headphone cue reach the sound card:\n"
           "\n"
           "Stereo (master only): master on outputs 1/2. No headphone cue.\n"
           "\n"
           "4-channel: master on outputs 1/2 (speakers), cue on outputs 3/4 (headphones). Needs an audio "
           "interface with at least 4 outputs.\n"
           "\n"
           "Split: for a single stereo output such as a laptop headphone jack. Your left ear hears the master "
           "(mixed to mono), your right ear hears the cue (mixed to mono).";
}

AudioSettingsComponent::AudioSettingsComponent(engine::AudioDeviceService& audioService,
                                               engine::AudioEngine& audioEngine, Callbacks cb)
    : audio(audioService)
    , engine(audioEngine)
    , callbacks(std::move(cb))
    , selector(audioService.getDeviceManager(),
               0,     // min inputs
               0,     // max inputs (no mic yet)
               1,     // min outputs
               4,     // max outputs (4-channel layout)
               false, // MIDI inputs
               false, // MIDI output
               true,  // channels as stereo pairs
               false) // advanced options visible
{
    layoutLabel.setText("Output layout:", juce::dontSendNotification);
    layoutLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(layoutLabel);

    for (int i = 0; i < engine::kNumOutputLayouts; ++i)
        layoutBox.addItem(engine::outputLayoutName(static_cast<engine::OutputLayout>(i)), i + 1);
    layoutBox.setSelectedId(static_cast<int>(callbacks.getLayout()) + 1, juce::dontSendNotification);
    layoutBox.setTooltip(layoutTooltip());
    layoutLabel.setTooltip(layoutTooltip());
    layoutBox.onChange = [this]
    {
        const auto id = layoutBox.getSelectedId();
        if (id > 0)
            callbacks.setLayout(static_cast<engine::OutputLayout>(id - 1));
    };
    addAndMakeVisible(layoutBox);

    addAndMakeVisible(selector);

    toneLabel.setText("Test tone:", juce::dontSendNotification);
    toneLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(toneLabel);
    toneMaster.setTooltip("440 Hz on the master bus (speakers / left ear in split mode)");
    toneCue.setTooltip("440 Hz on the cue bus (headphones / right ear in split mode)");
    toneMaster.onClick = [this] { playTone(engine::Bus::Master); };
    toneCue.onClick = [this] { playTone(engine::Bus::Cue); };
    toneStop.onClick = [this] { stopTone(); };
    addAndMakeVisible(toneMaster);
    addAndMakeVisible(toneCue);
    addAndMakeVisible(toneStop);

    status.setJustificationType(juce::Justification::topLeft);
    status.setFont(juce::FontOptions(14.0f));
    addAndMakeVisible(status);

    setSize(560, 520);
    timerCallback();
    startTimerHz(4);
}

AudioSettingsComponent::~AudioSettingsComponent()
{
    stopTone(); // don't leave a tone running after the window closes
}

void AudioSettingsComponent::playTone(engine::Bus bus)
{
    toneOn = true;
    engine.send(engine::SetTestTone{true, 440.0f, kToneGainDb, bus, 0.0f});
}

void AudioSettingsComponent::stopTone()
{
    if (!toneOn)
        return;
    toneOn = false;
    engine.send(engine::SetTestTone{false, 440.0f, kToneGainDb, engine::Bus::Master, 0.0f});
}

void AudioSettingsComponent::resized()
{
    auto area = getLocalBounds().reduced(8);

    auto layoutRow = area.removeFromTop(kRowHeight);
    layoutLabel.setBounds(layoutRow.removeFromLeft(kLabelWidth));
    layoutBox.setBounds(layoutRow.reduced(4, 2));
    area.removeFromTop(4);

    status.setBounds(area.removeFromBottom(kStatusHeight));
    auto toneRow = area.removeFromBottom(kRowHeight);
    toneLabel.setBounds(toneRow.removeFromLeft(kLabelWidth));
    toneRow.reduce(4, 2);
    const int w = toneRow.getWidth() / 3;
    toneMaster.setBounds(toneRow.removeFromLeft(w).reduced(2, 0));
    toneCue.setBounds(toneRow.removeFromLeft(w).reduced(2, 0));
    toneStop.setBounds(toneRow.reduced(2, 0));

    selector.setBounds(area);
}

void AudioSettingsComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void AudioSettingsComponent::timerCallback()
{
    const auto s = audio.getStatus();
    const auto& e = engine.readState();
    if (!s.isOpen)
    {
        status.setText("No audio device open" + (s.lastError.isEmpty() ? juce::String() : ": " + s.lastError),
                       juce::dontSendNotification);
        return;
    }

    juce::String routing =
        juce::String(engine::outputLayoutName(e.outputLayout)) + " on " + juce::String(e.outputChannels) + " outputs";
    if (e.outputLayout != engine::OutputLayout::StereoMaster && !e.cueRouted)
        routing += "  -  cue NOT audible: this device has fewer outputs than the layout needs";

    status.setText(juce::String::formatted("Latency %.1f ms (buffer %.1f ms + driver %.1f ms)\n"
                                           "Xruns: %d    Audio CPU: %.0f %%\n",
                                           s.totalOutputLatencyMs, s.bufferMs, s.outputLatencyMs, s.xruns,
                                           100.0 * s.cpuLoad) +
                       routing,
                   juce::dontSendNotification);
}

void showAudioSettingsWindow(engine::AudioDeviceService& audio, engine::AudioEngine& engine,
                             AudioSettingsComponent::Callbacks callbacks, juce::Component* parent)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new AudioSettingsComponent(audio, engine, std::move(callbacks))); // JUCE takes ownership
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
