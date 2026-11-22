#include "ui/AutomixPanel.h"

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kOn(0xff3cc46a);
const juce::Colour kText(0xffe8e8ee);
const juce::Colour kDim(0xff8a8a96);

const char* styleName(int style)
{
    return style == 1 ? "FILTER OUT" : style == 2 ? "ECHO OUT" : "BLEND";
}
} // namespace

AutomixPanel::AutomixPanel()
{
    for (auto* b : {&automix, &queue, &repeat, &random, &beats, &style})
    {
        b->setColour(juce::TextButton::buttonColourId, kPanel);
        b->setColour(juce::TextButton::buttonOnColourId, kOn);
        b->setColour(juce::TextButton::textColourOffId, kText);
        b->setColour(juce::TextButton::textColourOnId, juce::Colours::black);
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(*b);
    }
    automix.setTooltip("Automix on / off (F10): plays the queue, mixing each track into the next");
    automix.onClick = [this]
    {
        if (onToggle)
            onToggle();
    };
    queue.setTooltip("The browser's selected tracks become the queue (in the order shown)");
    queue.onClick = [this]
    {
        if (onQueue)
            onQueue();
    };
    repeat.setTooltip("Round again after the last track");
    repeat.onClick = [this]
    {
        current.repeat = !current.repeat;
        changed();
    };
    random.setTooltip("A shuffled order (each track once)");
    random.onClick = [this]
    {
        current.random = !current.random;
        changed();
    };
    beats.setTooltip("Each transition's length");
    beats.onClick = [this]
    {
        current.beats = current.beats == 8 ? 16 : current.beats == 16 ? 32 : 8;
        changed();
    };
    style.setTooltip("Each transition's style: a blend, with a filter sweep out, or ending on an echo out");
    style.onClick = [this]
    {
        current.style = (current.style + 1) % 3;
        changed();
    };
    setSettings({});
}

void AutomixPanel::setSettings(const Settings& s)
{
    current = s;
    repeat.setToggleState(current.repeat, juce::dontSendNotification);
    random.setToggleState(current.random, juce::dontSendNotification);
    beats.setButtonText(juce::String(current.beats) + " BEATS");
    style.setButtonText(styleName(current.style));
}

void AutomixPanel::changed()
{
    setSettings(current);
    if (onSettingsChanged)
        onSettingsChanged(current);
}

void AutomixPanel::setStatus(bool on, const juce::String& text)
{
    automix.setToggleState(on, juce::dontSendNotification);
    if (text != status)
    {
        status = text;
        repaint(statusArea);
    }
}

void AutomixPanel::resized()
{
    auto area = getLocalBounds().reduced(6);
    automix.setBounds(area.removeFromLeft(96).reduced(2));
    area.removeFromLeft(6);
    auto options = area.removeFromRight(330);
    auto top = options.removeFromTop(options.getHeight() / 2);
    queue.setBounds(top.removeFromLeft(110).reduced(2));
    repeat.setBounds(top.removeFromLeft(110).reduced(2));
    random.setBounds(top.reduced(2));
    beats.setBounds(options.removeFromLeft(165).reduced(2));
    style.setBounds(options.reduced(2));
    statusArea = area.reduced(4, 0);
}

void AutomixPanel::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    g.setColour(automix.getToggleState() ? kText : kDim);
    g.setFont(juce::FontOptions(13.0f));
    g.drawFittedText(status, statusArea, juce::Justification::centredLeft, 3);
}

} // namespace devildj::ui
