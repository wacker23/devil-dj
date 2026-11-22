#include "ui/RecorderPanel.h"

#include <cmath>

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kRecColour(0xffe0483c);
const juce::Colour kText(0xffe8e8ee);
const juce::Colour kDim(0xff8a8a96);

void style(juce::TextButton& b)
{
    b.setColour(juce::TextButton::buttonColourId, kPanel);
    b.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff3a3a46));
    b.setColour(juce::TextButton::textColourOffId, kText);
    b.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    b.setWantsKeyboardFocus(false);
}

juce::String clock(double seconds)
{
    const auto s = static_cast<int>(std::floor(seconds));
    return juce::String(s / 3600) + ":" + juce::String(s / 60 % 60).paddedLeft('0', 2) + ":" +
           juce::String(s % 60).paddedLeft('0', 2);
}
} // namespace

RecorderPanel::RecorderPanel(engine::AudioEngine& e, engine::Recorder& r)
    : engine(e)
    , recorder(r)
    , vblank(this, [this] { refresh(); })
{
    for (auto* b : {&rec, &format, &source, &autoStart, &autoSplit, &folder})
    {
        style(*b);
        addAndMakeVisible(*b);
    }
    rec.setColour(juce::TextButton::buttonOnColourId, kRecColour);
    rec.setTooltip("Record the mix (F9): start / stop");
    rec.onClick = [this]
    {
        if (onRecord)
            onRecord(!recorder.isRunning());
    };
    format.setTooltip("24-bit WAV or FLAC (the next recording)");
    format.onClick = [this]
    {
        current.flac = !current.flac;
        changed();
    };
    source.setTooltip("Record the master, or what is in the headphones");
    source.onClick = [this]
    {
        current.headphones = !current.headphones;
        changed();
    };
    autoStart.setTooltip("The file begins with the first sound (after REC, it waits for it)");
    autoStart.onClick = [this]
    {
        current.autoStart = !current.autoStart;
        changed();
    };
    autoSplit.setTooltip("20 s of silence ends a file; the next sound begins a new one");
    autoSplit.onClick = [this]
    {
        current.autoSplit = !current.autoSplit;
        changed();
    };
    folder.setTooltip("Open the recordings folder (Documents\\Devil-DJ\\Recordings)");
    folder.onClick = [this]
    {
        if (onOpenFolder)
            onOpenFolder();
    };
    setSettings({});
}

RecorderPanel::~RecorderPanel() = default;

void RecorderPanel::setSettings(const Settings& s)
{
    current = s;
    format.setButtonText(current.flac ? "FLAC" : "WAV");
    source.setButtonText(current.headphones ? "PHONES" : "MASTER");
    autoStart.setToggleState(current.autoStart, juce::dontSendNotification);
    autoSplit.setToggleState(current.autoSplit, juce::dontSendNotification);
    repaint();
}

void RecorderPanel::changed()
{
    setSettings(current);
    if (onSettingsChanged)
        onSettingsChanged(current);
}

juce::String RecorderPanel::statusText() const
{
    if (!recorder.isRunning())
        return recorder.filesWritten() > 0 ? "stopped" : "not recording";
    if (recorder.isWaiting())
        return "waiting for sound";
    return "recording";
}

void RecorderPanel::refresh()
{
    rec.setToggleState(recorder.isRunning(), juce::dontSendNotification);
    rec.setButtonText(recorder.isRunning() ? "STOP" : "REC");
    repaint(infoArea.getUnion(meterArea));
}

void RecorderPanel::resized()
{
    auto area = getLocalBounds().reduced(6);
    rec.setBounds(area.removeFromLeft(84).reduced(2));
    area.removeFromLeft(6);
    auto options = area.removeFromRight(260);
    auto top = options.removeFromTop(options.getHeight() / 2);
    format.setBounds(top.removeFromLeft(60).reduced(2));
    source.setBounds(top.removeFromLeft(80).reduced(2));
    folder.setBounds(top.reduced(2));
    autoStart.setBounds(options.removeFromLeft(130).reduced(2));
    autoSplit.setBounds(options.reduced(2));
    area.removeFromRight(8);
    meterArea = area.removeFromBottom(14);
    infoArea = area;
}

void RecorderPanel::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    const auto& s = engine.readState();
    const double rate = s.sampleRate > 0.0 ? s.sampleRate : 48000.0;
    g.setColour(recorder.isWriting() ? kRecColour : kText);
    g.setFont(juce::FontOptions(26.0f, juce::Font::bold));
    auto info = infoArea;
    g.drawText(clock(static_cast<double>(recorder.framesInFile()) / rate), info.removeFromLeft(150),
               juce::Justification::centredLeft);
    g.setFont(juce::FontOptions(12.0f));
    g.setColour(kDim);
    const auto file = recorder.currentFile();
    g.drawText(statusText() + (file != juce::File() ? "  -  " + file.getFileName() : juce::String()), info,
               juce::Justification::centredLeft, true);
    // The level of what is recorded (the master's or the headphones' meter, after its limiter).
    const float peak = current.headphones ? std::max(s.cuePeakL, s.cuePeakR) : std::max(s.masterPeakL, s.masterPeakR);
    const float db = peak > 0.0f ? 20.0f * std::log10(peak) : -100.0f;
    const float fraction = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
    g.setColour(kPanel);
    g.fillRoundedRectangle(meterArea.toFloat(), 3.0f);
    g.setColour(db > -1.0f ? kRecColour : juce::Colour(0xff3cc46a));
    g.fillRoundedRectangle(meterArea.toFloat().withWidth(meterArea.getWidth() * fraction), 3.0f);
}

} // namespace devildj::ui
