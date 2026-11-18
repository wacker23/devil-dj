#pragma once

#include "engine/AudioDeviceService.h"
#include "engine/AudioEngine.h"
#include "engine/BufferRegistry.h"
#include "engine/TrackLoader.h"
#include "ui/DebugOverlay.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>

namespace devildj::app
{

// Root component of the main window. Decks, mixer and browser are added later in Phase 1.
// Hosts the debug overlay (F12) for now.
class MainComponent final : public juce::Component
{
public:
    MainComponent(engine::AudioEngine& engine, engine::AudioDeviceService& audio, engine::TrackLoader& loader,
                  engine::BufferRegistry& registry);
    ~MainComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

    void toggleDebugOverlay();
    void toggleTestTone();

    // Temporary until the file browser (P1-18) and library (Phase 2): pick a file and load it.
    void chooseTrackForDeck(engine::DeckId deck);
    void unloadDeck(engine::DeckId deck);

private:
    juce::StringArray statusLines() const;

    engine::AudioEngine& engine;
    engine::AudioDeviceService& audio;
    engine::TrackLoader& loader;
    engine::BufferRegistry& registry;
    std::array<juce::String, 2> deckStatus{"empty", "empty"};
    std::unique_ptr<juce::FileChooser> chooser;
    ui::DebugOverlay overlay;
    bool testToneOn = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace devildj::app
