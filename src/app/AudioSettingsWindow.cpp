#include "AudioSettingsWindow.h"

#include <algorithm>
#include <cmath>

namespace devildj::app
{

namespace
{
constexpr float kToneGainDb = -18.0f;
constexpr int kWidth = 680;
constexpr int kHeight = 580;
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kText(0xffe8e8ee);
const juce::Colour kDim(0xff9a9aa6);
const juce::Colour kAccent(0xffe0483c);
const juce::Colour kCue(0xfff0a030);
const juce::Colour kGood(0xff3cc46a);

struct LayoutInfo
{
    const char* title;
    const char* line;
};
const LayoutInfo kLayouts[engine::kNumOutputLayouts] = {
    {"Speakers only", "Master on outputs 1/2. No headphone cue."},
    {"4-channel interface", "Master on outputs 1/2 (speakers), cue on outputs 3/4 (headphones)."},
    {"Split cable", "One stereo output (laptop jack): left ear = master, right ear = cue."},
};

void styleBox(juce::ComboBox& b)
{
    b.setColour(juce::ComboBox::backgroundColourId, kPanel);
    b.setColour(juce::ComboBox::textColourId, kText);
    b.setColour(juce::ComboBox::outlineColourId, juce::Colours::white.withAlpha(0.15f));
    b.setColour(juce::ComboBox::arrowColourId, kDim);
}
} // namespace

juce::String AudioSettingsComponent::bufferText(int samples, double sampleRate)
{
    const double ms = sampleRate > 0.0 ? 1000.0 * samples / sampleRate : 0.0;
    return juce::String(samples) + " samples  (" + juce::String(ms, 1) + " ms)";
}

AudioSettingsComponent::LatencyRating AudioSettingsComponent::rateLatency(double totalMs) noexcept
{
    return totalMs <= 12.0 ? LatencyRating::Great : totalMs <= 25.0 ? LatencyRating::Good : LatencyRating::High;
}

juce::String AudioSettingsComponent::layoutTooltip()
{
    return "How the master mix and your headphone cue reach the sound card:\n\n"
           "Speakers only: master on outputs 1/2. No headphone cue.\n\n"
           "4-channel interface: master on outputs 1/2 (speakers), cue on outputs 3/4 (headphones). Needs an "
           "audio interface with at least 4 outputs.\n\n"
           "Split cable: for a single stereo output such as a laptop headphone jack, with a splitter cable or "
           "headphones only. Left ear = the master (in mono), right ear = the cue (in mono).";
}

AudioSettingsComponent::AudioSettingsComponent(engine::AudioDeviceService& audioService,
                                               engine::AudioEngine& audioEngine, Callbacks cb)
    : audio(audioService)
    , engine(audioEngine)
    , callbacks(std::move(cb))
{
    for (auto* b : {&driver, &device, &rate, &buffer})
    {
        styleBox(*b);
        addAndMakeVisible(b);
    }
    driver.setTooltip("Windows Audio is the safe choice. 'Exclusive Mode' and 'Low Latency Mode' give a shorter delay "
                      "but take the sound card for Devil-DJ alone. ASIO drivers appear here if installed.");
    device.setTooltip("The sound card or audio interface to play through.");
    rate.setTooltip("48000 Hz is a good default. Higher rates use more CPU.");
    buffer.setTooltip("Smaller buffer = shorter delay between your hands and the sound, but more CPU. If you hear "
                      "crackles or the glitch counter goes up, choose a bigger one.");

    driver.onChange = [this]
    {
        if (driver.getText().isNotEmpty() && driver.getText() != audio.getCurrentDeviceType())
            showError(audio.setDeviceType(driver.getText()));
        refreshFromDevice();
    };
    device.onChange = [this]
    {
        if (device.getText().isNotEmpty() && device.getText() != audio.getStatus().deviceName)
            showError(audio.setOutputDevice(device.getText()));
        refreshFromDevice();
    };
    rate.onChange = [this]
    {
        if (const int id = rate.getSelectedId(); id > 0 && id != juce::roundToInt(audio.getStatus().sampleRate))
            showError(audio.setSampleRate(id));
        refreshFromDevice();
    };
    buffer.onChange = [this]
    {
        if (const int id = buffer.getSelectedId(); id > 0 && id != audio.getStatus().bufferSize)
            showError(audio.setBufferSize(id));
        refreshFromDevice();
    };

    bufferHint.setText("Start with 256. Crackles? Pick a bigger buffer.", juce::dontSendNotification);
    bufferHint.setColour(juce::Label::textColourId, kDim);
    bufferHint.setFont(juce::FontOptions(13.0f));
    addAndMakeVisible(bufferHint);
    errorLabel.setColour(juce::Label::textColourId, kAccent);
    errorLabel.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    addAndMakeVisible(errorLabel);

    for (int i = 0; i < engine::kNumOutputLayouts; ++i)
    {
        auto& b = layoutButtons[static_cast<std::size_t>(i)];
        b.setButtonText(kLayouts[i].title);
        b.setClickingTogglesState(false);
        b.setColour(juce::TextButton::buttonColourId, kPanel);
        b.setColour(juce::TextButton::buttonOnColourId, kAccent.withAlpha(0.85f));
        b.setColour(juce::TextButton::textColourOffId, kText);
        b.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        b.setTooltip(layoutTooltip());
        b.onClick = [this, i] { selectLayout(static_cast<engine::OutputLayout>(i)); };
        addAndMakeVisible(b);
    }

    for (auto* b : {&toneSpeakers, &toneHeadphones, &toneStop, &resetGlitches})
    {
        b->setColour(juce::TextButton::buttonColourId, kPanel);
        b->setColour(juce::TextButton::textColourOffId, kText);
        addAndMakeVisible(b);
    }
    toneSpeakers.setTooltip("A quiet 440 Hz beep on the master (speakers; left ear with a split cable)");
    toneHeadphones.setTooltip("A quiet 440 Hz beep on the cue (headphones; right ear with a split cable)");
    toneSpeakers.onClick = [this] { playTone(engine::Bus::Master); };
    toneHeadphones.onClick = [this] { playTone(engine::Bus::Cue); };
    toneStop.onClick = [this] { stopTone(); };
    resetGlitches.setTooltip("Start counting glitches (dropouts) from zero");
    resetGlitches.onClick = [this]
    {
        glitchBaseline = status.xruns;
        repaint();
    };

    status = audio.getStatus();
    glitchBaseline = status.xruns;
    refreshFromDevice();
    setSize(kWidth, kHeight);
    startTimerHz(4);
}

AudioSettingsComponent::~AudioSettingsComponent()
{
    stopTone(); // don't leave a tone running after the window closes
}

int AudioSettingsComponent::glitchesSinceReset() const noexcept
{
    return std::max(0, status.xruns - glitchBaseline);
}

void AudioSettingsComponent::refreshFromDevice()
{
    status = audio.getStatus();
    const auto typeName = audio.getCurrentDeviceType();

    driver.clear(juce::dontSendNotification);
    const auto types = audio.getDeviceTypeNames();
    for (int i = 0; i < types.size(); ++i)
        driver.addItem(types[i], i + 1);
    driver.setText(typeName, juce::dontSendNotification);

    device.clear(juce::dontSendNotification);
    const auto devices = audio.getOutputDeviceNames(typeName);
    for (int i = 0; i < devices.size(); ++i)
        device.addItem(devices[i], i + 1);
    device.setText(status.deviceName, juce::dontSendNotification);

    rate.clear(juce::dontSendNotification);
    for (double r : audio.getAvailableSampleRates())
        rate.addItem(juce::String(juce::roundToInt(r)) + " Hz", juce::roundToInt(r));
    rate.setSelectedId(juce::roundToInt(status.sampleRate), juce::dontSendNotification);

    buffer.clear(juce::dontSendNotification);
    for (int size : audio.getAvailableBufferSizes())
        buffer.addItem(bufferText(size, status.sampleRate), size);
    buffer.setSelectedId(status.bufferSize, juce::dontSendNotification);

    const auto layout = callbacks.getLayout ? callbacks.getLayout() : engine::OutputLayout::StereoMaster;
    for (int i = 0; i < engine::kNumOutputLayouts; ++i)
        layoutButtons[static_cast<std::size_t>(i)].setToggleState(static_cast<int>(layout) == i,
                                                                  juce::dontSendNotification);
    shownSetup = typeName + "|" + status.deviceName + "|" + juce::String(status.sampleRate) + "|" +
                 juce::String(status.bufferSize);
    repaint();
}

void AudioSettingsComponent::selectLayout(engine::OutputLayout layout)
{
    if (callbacks.setLayout)
        callbacks.setLayout(layout);
    refreshFromDevice();
}

void AudioSettingsComponent::showError(const juce::String& error)
{
    errorLabel.setText(error.isEmpty() ? juce::String() : "Couldn't change that: " + error, juce::dontSendNotification);
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

void AudioSettingsComponent::timerCallback()
{
    status = audio.getStatus();
    const auto setup = audio.getCurrentDeviceType() + "|" + status.deviceName + "|" + juce::String(status.sampleRate) +
                       "|" + juce::String(status.bufferSize);
    if (setup != shownSetup) // changed elsewhere (device unplugged, another window): show what's real
        refreshFromDevice();
    repaint(readouts);
    repaint(section2);
}

void AudioSettingsComponent::resized()
{
    auto area = getLocalBounds().reduced(16);
    constexpr int labelWidth = 130, row = 30;

    section1 = area.removeFromTop(28 + 4 * (row + 6) + 24);
    {
        auto s = section1.withTrimmedTop(28);
        juce::ComboBox* boxes[] = {&driver, &device, &rate, &buffer};
        for (int i = 0; i < 4; ++i)
        {
            auto r = s.removeFromTop(row);
            rowLabels[static_cast<std::size_t>(i)] = r.removeFromLeft(labelWidth);
            boxes[i]->setBounds(r.withWidth(std::min(r.getWidth(), 420)));
            s.removeFromTop(6);
        }
        auto hintRow = s.removeFromTop(22).withTrimmedLeft(labelWidth);
        bufferHint.setBounds(hintRow);
    }
    area.removeFromTop(10);

    section2 = area.removeFromTop(28 + 92);
    {
        auto s = section2.withTrimmedTop(28);
        const int w = (s.getWidth() - 2 * 8) / 3;
        for (int i = 0; i < engine::kNumOutputLayouts; ++i)
        {
            auto card = s.removeFromLeft(w);
            s.removeFromLeft(8);
            layoutButtons[static_cast<std::size_t>(i)].setBounds(card.removeFromTop(30));
        }
    }
    area.removeFromTop(10);

    section3 = area;
    {
        auto s = section3.withTrimmedTop(28);
        auto tones = s.removeFromTop(32);
        toneSpeakers.setBounds(tones.removeFromLeft(180));
        tones.removeFromLeft(8);
        toneHeadphones.setBounds(tones.removeFromLeft(180));
        tones.removeFromLeft(8);
        toneStop.setBounds(tones.removeFromLeft(110));
        s.removeFromTop(10);
        readouts = s.removeFromTop(110);
        resetGlitches.setBounds(readouts.getX() + 220 + 110, readouts.getY() + 28, 70, 26); // beside the count
        errorLabel.setBounds(s.removeFromTop(24));
    }
}

void AudioSettingsComponent::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    const auto heading = [&g](juce::Rectangle<int> r, const juce::String& number, const juce::String& text)
    {
        auto line = r.removeFromTop(24);
        g.setColour(kAccent);
        g.fillEllipse(line.removeFromLeft(24).toFloat().reduced(1.0f));
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
        g.drawText(number, line.withX(r.getX()).withWidth(24), juce::Justification::centred);
        g.setFont(juce::FontOptions(17.0f, juce::Font::bold));
        g.drawText(text, line.withTrimmedLeft(8), juce::Justification::centredLeft);
    };
    heading(section1, "1", "Sound card");
    heading(section2, "2", "Headphones");
    heading(section3, "3", "Check");

    g.setColour(kDim);
    g.setFont(juce::FontOptions(14.0f));
    const char* const labels[] = {"Driver", "Output device", "Sample rate", "Buffer size"};
    for (int i = 0; i < 4; ++i)
        g.drawText(labels[i], rowLabels[static_cast<std::size_t>(i)], juce::Justification::centredLeft);

    // Layout cards: one line of explanation and where each sound goes.
    const auto& e = engine.readState();
    for (int i = 0; i < engine::kNumOutputLayouts; ++i)
    {
        const auto button = layoutButtons[static_cast<std::size_t>(i)].getBounds();
        auto card = juce::Rectangle<int>(button.getX(), button.getBottom() + 4, button.getWidth(),
                                         section2.getBottom() - button.getBottom() - 4);
        g.setColour(kPanel);
        g.fillRoundedRectangle(card.toFloat(), 6.0f);
        auto inner = card.reduced(8, 4);
        g.setColour(kDim);
        g.setFont(juce::FontOptions(12.0f));
        g.drawFittedText(kLayouts[i].line, inner.removeFromTop(30), juce::Justification::topLeft, 2);
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        const auto chip = [&g, &inner](const juce::String& text, juce::Colour c)
        {
            auto r = inner.removeFromLeft(std::min(inner.getWidth(), 18 + static_cast<int>(text.length()) * 7));
            g.setColour(c.withAlpha(0.25f));
            g.fillRoundedRectangle(r.toFloat().reduced(0.0f, 2.0f), 4.0f);
            g.setColour(c);
            g.drawText(text, r, juce::Justification::centred);
            inner.removeFromLeft(6);
        };
        if (i == static_cast<int>(engine::OutputLayout::StereoMaster))
            chip("1/2: master", kGood);
        else if (i == static_cast<int>(engine::OutputLayout::FourChannel))
        {
            chip("1/2: master", kGood);
            chip("3/4: cue", kCue);
        }
        else
        {
            chip("L: master", kGood);
            chip("R: cue", kCue);
        }
    }
    // Warn when the chosen layout can't reach the headphones on this device.
    if (e.outputLayout == engine::OutputLayout::FourChannel && !e.cueRouted)
    {
        g.setColour(kCue);
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText("This device has only " + juce::String(audio.getMaxOutputChannelCount()) +
                       " outputs: the headphones won't hear the cue. Try Split cable.",
                   section2.withTrimmedLeft(140).removeFromTop(24), juce::Justification::centredRight);
    }

    // Readouts.
    auto r = readouts;
    auto left = r.removeFromLeft(200);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText("Delay (hands to ears)", left.removeFromTop(18), juce::Justification::centredLeft);
    const auto rating = rateLatency(status.totalOutputLatencyMs);
    const auto colour = rating == LatencyRating::Great ? kGood : rating == LatencyRating::Good ? kCue : kAccent;
    g.setColour(status.isOpen ? colour : kDim);
    g.setFont(juce::FontOptions(34.0f, juce::Font::bold));
    g.drawText(status.isOpen ? juce::String(status.totalOutputLatencyMs, 1) + " ms" : juce::String("--"),
               left.removeFromTop(42), juce::Justification::centredLeft);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(!status.isOpen                   ? "No sound card open"
               : rating == LatencyRating::Great ? "Great: fine for scratching"
               : rating == LatencyRating::Good  ? "Good: fine for mixing"
                                                : "High: try a smaller buffer",
               left.removeFromTop(18), juce::Justification::centredLeft);
    g.setColour(kDim);
    g.drawText("buffer " + juce::String(status.bufferMs, 1) + " ms + driver " +
                   juce::String(status.outputLatencyMs, 1) + " ms",
               left.removeFromTop(18), juce::Justification::centredLeft);

    auto right = r.withTrimmedLeft(20);
    g.setColour(kDim);
    g.drawText("Glitches (dropouts)", right.removeFromTop(18).withWidth(200), juce::Justification::centredLeft);
    const int glitches = glitchesSinceReset();
    g.setColour(glitches == 0 ? kGood : kAccent);
    g.setFont(juce::FontOptions(34.0f, juce::Font::bold));
    g.drawText(juce::String(glitches), right.removeFromTop(42).withWidth(100), juce::Justification::centredLeft);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(glitches == 0 ? "None. Play a track for a while to be sure." : "Crackles? Pick a bigger buffer.",
               right.removeFromTop(18), juce::Justification::centredLeft);
    g.drawText("Audio CPU " + juce::String(juce::roundToInt(100.0 * status.cpuLoad)) + " %    " +
                   juce::String(juce::roundToInt(status.sampleRate)) + " Hz, " + juce::String(status.bufferSize) +
                   " samples, " + juce::String(e.outputChannels) + " outputs",
               right.removeFromTop(18), juce::Justification::centredLeft);
}

void showAudioSettingsWindow(engine::AudioDeviceService& audio, engine::AudioEngine& engine,
                             AudioSettingsComponent::Callbacks callbacks, juce::Component* parent)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new AudioSettingsComponent(audio, engine, std::move(callbacks))); // JUCE takes ownership
    options.dialogTitle = "Audio settings";
    options.componentToCentreAround = parent;
    options.dialogBackgroundColour = kBackground;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

} // namespace devildj::app
