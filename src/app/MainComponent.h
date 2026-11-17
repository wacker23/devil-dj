#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace devildj::app
{

// Root component of the main window. Empty for now; decks, mixer and browser are added in Phase 1.
class MainComponent final : public juce::Component
{
public:
    MainComponent();

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace devildj::app
