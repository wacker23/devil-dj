#include "MainComponent.h"

namespace devildj::app
{

MainComponent::MainComponent()
{
    setSize(1280, 800);
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    g.setColour(juce::Colours::white.withAlpha(0.6f));
    g.setFont(juce::FontOptions(32.0f, juce::Font::bold));
    g.drawText("Devil-DJ", getLocalBounds(), juce::Justification::centred);
}

void MainComponent::resized() {}

} // namespace devildj::app
