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
    bool keyStateChanged(bool isKeyDown) override;

    void toggleDebugOverlay();
    void toggleTestTone();

    // Temporary until the file browser (P1-18) and library (Phase 2): pick a file and load it.
    void chooseTrackForDeck(engine::DeckId deck);
    void unloadDeck(engine::DeckId deck);

private:
    juce::StringArray statusLines();
    void handleDeckKeys();

    engine::AudioEngine& engine;
    engine::AudioDeviceService& audio;
    engine::TrackLoader& loader;
    engine::BufferRegistry& registry;
    std::array<juce::String, 2> deckStatus{"empty", "empty"};
    std::unique_ptr<juce::FileChooser> chooser;
    ui::DebugOverlay overlay;
    bool testToneOn = false;

    // Temporary deck keys (KEYMAP.md) until the KeyboardController (P1-16): Z/X deck A, M/, deck B.
    //   Z/X cue/play A, M/, cue/play B; 1/2 and 7/8 pitch bend (hold); Shift+1/2, Shift+7/8 tempo -/+ 0.1 %.
    enum class KeyRole
    {
        Cue,
        Play,
        BendDown,
        BendUp,
    };
    struct DeckKey
    {
        int keyCode;
        engine::DeckId deck;
        KeyRole role;
        bool down = false;
    };
    std::array<DeckKey, 8> deckKeys{{{'Z', 0, KeyRole::Cue},
                                     {'X', 0, KeyRole::Play},
                                     {'1', 0, KeyRole::BendDown},
                                     {'2', 0, KeyRole::BendUp},
                                     {'M', 1, KeyRole::Cue},
                                     {',', 1, KeyRole::Play},
                                     {'7', 1, KeyRole::BendDown},
                                     {'8', 1, KeyRole::BendUp}}};

public:
    void resetTempo(engine::DeckId deck);
    void setTempoRange(double rangePercent);
    void setHighQualityResampling(bool on);
    void toggleKeyLock(engine::DeckId deck);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace devildj::app
