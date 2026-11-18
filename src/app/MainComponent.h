#pragma once

#include "engine/AudioDeviceService.h"
#include "engine/AudioEngine.h"
#include "ui/DebugOverlay.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace devildj::app
{

// Root component of the main window. Decks, mixer and browser are added later in Phase 1.
// Hosts the debug overlay (F12) for now.
class MainComponent final : public juce::Component
{
public:
    MainComponent(engine::AudioEngine& engine, engine::AudioDeviceService& audio);

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    void toggleDebugOverlay();
    void toggleTestTone();

private:
    engine::AudioEngine& engine;
    ui::DebugOverlay overlay;
    bool testToneOn = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace devildj::app
