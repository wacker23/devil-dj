#include "ui/BeatChannelStrip.h"

#include "dsp/fx/ColorFx.h"

#include <algorithm>

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kText(0xffe8e8ee);
const juce::Colour kDim(0xff8a8a96);
const juce::Colour kBeats(0xffe8a23c);
const juce::Colour kCue(0xffe0a030);

void style(juce::TextButton& b)
{
    b.setColour(juce::TextButton::buttonColourId, kPanel);
    b.setColour(juce::TextButton::buttonOnColourId, kBeats);
    b.setColour(juce::TextButton::textColourOffId, kText);
    b.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    b.setWantsKeyboardFocus(false);
}

constexpr double kMinBpm = 60.0, kMaxBpm = 200.0;
} // namespace

BeatChannelStrip::BeatChannelStrip(engine::AudioEngine& e)
    : engine(e)
{
    for (auto* b : {&cue, &assign, &tempo})
    {
        style(*b);
        addAndMakeVisible(*b);
    }
    cue.setColour(juce::TextButton::buttonOnColourId, kCue);
    cue.setTooltip("The Beat Maker in the headphones");
    cue.onClick = [this]
    {
        const bool on = engine.readState().channels[engine::kBeatMakerChannel].cue;
        sendParam(engine::ChannelParam::Cue, on ? 0.0f : 1.0f);
    };
    assign.setTooltip("The Beat Maker's crossfader side: A, THRU (not on the crossfader), B");
    assign.onClick = [this]
    {
        const int now = engine.readState().channels[engine::kBeatMakerChannel].crossfaderAssign;
        sendParam(engine::ChannelParam::CrossfaderAssign, static_cast<float>((now + 1) % 3));
    };
    tempo.setTooltip("FOLLOW: the Beat Maker plays on the master clock (the decks' tempo); OWN: at its own BPM");
    tempo.onClick = [this]
    {
        follow = !follow;
        setTempo(follow, ownBpm);
        if (onTempo)
            onTempo(follow, ownBpm);
    };
    level = std::make_unique<ValueControl>("BEATS", ValueControl::Style::VerticalFader, 1.0);
    level->setAccent(kBeats);
    level->valueText = [](double v) { return juce::String(juce::roundToInt(100.0 * v)) + " %"; };
    level->onChange = [this](double v) { sendParam(engine::ChannelParam::Fader, static_cast<float>(v)); };
    addAndMakeVisible(*level);
    filter = std::make_unique<ValueControl>("FILTER", ValueControl::Style::Knob, 0.5);
    filter->setAccent(juce::Colour(0xff4aa8e0));
    filter->onChange = [this](double v) { sendParam(engine::ChannelParam::Filter, static_cast<float>(v)); };
    addAndMakeVisible(*filter);
    bpm = std::make_unique<ValueControl>("BPM", ValueControl::Style::Knob, (120.0 - kMinBpm) / (kMaxBpm - kMinBpm));
    bpm->setAccent(kBeats);
    bpm->valueText = [](double v) { return juce::String(kMinBpm + v * (kMaxBpm - kMinBpm), 1) + " BPM"; };
    bpm->onChange = [this](double v)
    {
        ownBpm = std::round((kMinBpm + v * (kMaxBpm - kMinBpm)) * 10.0) / 10.0;
        if (onTempo)
            onTempo(follow, ownBpm);
        repaint();
    };
    addAndMakeVisible(*bpm);
    setTempo(true, 120.0);
}

void BeatChannelStrip::sendParam(engine::ChannelParam p, float value)
{
    engine.send(engine::SetParam{engine::channelParam(engine::kBeatMakerChannel, p), value});
}

void BeatChannelStrip::setTempo(bool f, double b)
{
    follow = f;
    ownBpm = std::clamp(b, kMinBpm, kMaxBpm);
    tempo.setButtonText(follow ? "FOLLOW" : "OWN");
    tempo.setToggleState(!follow, juce::dontSendNotification);
    if (!bpm->isBeingChanged())
        bpm->setValue((ownBpm - kMinBpm) / (kMaxBpm - kMinBpm), false);
    bpm->setEnabled(!follow);
    bpm->setAlpha(follow ? 0.4f : 1.0f);
    repaint();
}

void BeatChannelStrip::refresh()
{
    const auto& s = engine.readState();
    const auto& ch = s.channels[engine::kBeatMakerChannel];
    cue.setToggleState(ch.cue, juce::dontSendNotification);
    assign.setButtonText(ch.crossfaderAssign == 0 ? "A" : ch.crossfaderAssign == 2 ? "B" : "THRU");
    if (!level->isBeingChanged())
        level->setValue(ch.fader, false);
    if (!filter->isBeingChanged())
        filter->setValue(ch.filter, false);
    filter->setLabel(juce::String(dsp::fx::ColorFx::typeName(static_cast<dsp::fx::ColorFx::Type>(s.colorFx))).toUpperCase());
}

void BeatChannelStrip::resized()
{
    auto area = getLocalBounds().reduced(4);
    level->setBounds(area.removeFromRight(40));
    area.removeFromRight(4);
    auto top = area.removeFromTop(area.getHeight() / 2);
    cue.setBounds(top.removeFromTop(top.getHeight() / 3).reduced(0, 2));
    assign.setBounds(top.removeFromTop(top.getHeight() / 2).reduced(0, 2));
    filter->setBounds(top);
    tempo.setBounds(area.removeFromTop(28).reduced(0, 2));
    bpm->setBounds(area);
}

void BeatChannelStrip::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    g.setColour(kPanel);
    g.drawRect(getLocalBounds());
    g.setColour(follow ? kDim : kText);
    g.setFont(juce::FontOptions(10.0f));
    g.drawText(follow ? "master tempo" : juce::String(ownBpm, 1) + " BPM", getLocalBounds().removeFromBottom(14),
               juce::Justification::centred);
}

} // namespace devildj::ui
