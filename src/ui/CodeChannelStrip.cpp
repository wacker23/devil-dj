#include "ui/CodeChannelStrip.h"

#include "dsp/fx/ColorFx.h"

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kText(0xffe8e8ee);
const juce::Colour kDim(0xff8a8a96);
const juce::Colour kCode(0xff5fd38a);
const juce::Colour kCue(0xffe0a030);

void style(juce::TextButton& b)
{
    b.setColour(juce::TextButton::buttonColourId, kPanel);
    b.setColour(juce::TextButton::buttonOnColourId, kCode);
    b.setColour(juce::TextButton::textColourOffId, kText);
    b.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    b.setWantsKeyboardFocus(false);
}
} // namespace

CodeChannelStrip::CodeChannelStrip(engine::AudioEngine& e)
    : engine(e)
{
    for (auto* b : {&cue, &assign})
    {
        style(*b);
        addAndMakeVisible(*b);
    }
    cue.setColour(juce::TextButton::buttonOnColourId, kCue);
    cue.setTooltip("The code deck in the headphones");
    cue.onClick = [this]
    {
        const bool on = engine.readState().channels[engine::kCodeDeckChannel].cue;
        sendParam(engine::ChannelParam::Cue, on ? 0.0f : 1.0f);
    };
    assign.setTooltip("The code deck's crossfader side: A, THRU (not on the crossfader), B");
    assign.onClick = [this]
    {
        const int now = engine.readState().channels[engine::kCodeDeckChannel].crossfaderAssign;
        sendParam(engine::ChannelParam::CrossfaderAssign, static_cast<float>((now + 1) % 3));
    };
    level = std::make_unique<ValueControl>("CODE", ValueControl::Style::VerticalFader, 1.0);
    level->setAccent(kCode);
    level->valueText = [](double v) { return juce::String(juce::roundToInt(100.0 * v)) + " %"; };
    level->onChange = [this](double v) { sendParam(engine::ChannelParam::Fader, static_cast<float>(v)); };
    addAndMakeVisible(*level);
    filter = std::make_unique<ValueControl>("FILTER", ValueControl::Style::Knob, 0.5);
    filter->setAccent(juce::Colour(0xff4aa8e0));
    filter->onChange = [this](double v) { sendParam(engine::ChannelParam::Filter, static_cast<float>(v)); };
    addAndMakeVisible(*filter);
}

void CodeChannelStrip::sendParam(engine::ChannelParam p, float value)
{
    engine.send(engine::SetParam{engine::channelParam(engine::kCodeDeckChannel, p), value});
}

void CodeChannelStrip::setStatus(const juce::String& text)
{
    if (text == statusText)
        return;
    statusText = text;
    repaint();
}

void CodeChannelStrip::refresh()
{
    const auto& s = engine.readState();
    const auto& ch = s.channels[engine::kCodeDeckChannel];
    cue.setToggleState(ch.cue, juce::dontSendNotification);
    assign.setButtonText(ch.crossfaderAssign == 0 ? "A" : ch.crossfaderAssign == 2 ? "B" : "THRU");
    if (!level->isBeingChanged())
        level->setValue(ch.fader, false);
    if (!filter->isBeingChanged())
        filter->setValue(ch.filter, false);
    filter->setLabel(juce::String(dsp::fx::ColorFx::typeName(static_cast<dsp::fx::ColorFx::Type>(s.colorFx))).toUpperCase());
}

void CodeChannelStrip::resized()
{
    auto area = getLocalBounds().reduced(4);
    area.removeFromBottom(28); // the status
    level->setBounds(area.removeFromRight(40));
    area.removeFromRight(4);
    cue.setBounds(area.removeFromTop(28).reduced(0, 2));
    assign.setBounds(area.removeFromTop(28).reduced(0, 2));
    filter->setBounds(area);
}

void CodeChannelStrip::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    g.setColour(kPanel);
    g.drawRect(getLocalBounds());
    g.setColour(statusText.containsIgnoreCase("error") || statusText.containsIgnoreCase("unknown") ? kCue : kDim);
    g.setFont(juce::FontOptions(10.0f));
    g.drawFittedText(statusText, getLocalBounds().reduced(4, 2).removeFromBottom(28), juce::Justification::centred, 2);
}

} // namespace devildj::ui
