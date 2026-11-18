#pragma once

#include "engine/AudioDeviceService.h"
#include "engine/AudioEngine.h"
#include "engine/BufferRegistry.h"
#include "engine/TrackLoader.h"
#include "ui/Actions.h"
#include "ui/DebugOverlay.h"
#include "ui/DeckView.h"
#include "ui/KeyboardController.h"
#include "ui/MixerView.h"
#include "ui/WaveformView.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>

namespace devildj::app
{

// Root component of the main window: deck views on top, zoomed waveforms below; mixer and browser later.
// Hosts the debug overlay (F12) for now.
class MainComponent final : public juce::Component
{
public:
    MainComponent(engine::AudioEngine& engine, engine::AudioDeviceService& audio, engine::TrackLoader& loader,
                  engine::BufferRegistry& registry);
    ~MainComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void toggleDebugOverlay();
    void toggleTestTone();

    // Temporary until the file browser (P1-18) and library (Phase 2): pick a file and load it.
    void chooseTrackForDeck(engine::DeckId deck);
    void unloadDeck(engine::DeckId deck);

private:
    juce::StringArray statusLines();
    // Keys (P1-16): every action is a CommandId; the KeyboardController turns physical keys into actions.
    void registerActions();
    bool onPhysicalKey(const ui::KeyboardController::KeyEvent& e);
    ui::ActionDispatcher actions;
    ui::KeyboardController keyboard{actions};
    std::unique_ptr<ui::PhysicalKeyHook> keyHook;
    // Held keys: filter sweeps and crossfader glides move every frame.
    void sweepHeldKeys();
    void sendCrossfader();
    double crossfader = 0.5;
    void sendFilter(int channel);
    std::array<double, 2> filterKnob{0.5, 0.5};
    std::array<bool, 2> filterSweeping{};
    bool crossfaderGliding = false;
    double lastSweepSeconds = 0.0;
    juce::VBlankAttachment heldKeySweep{this, [this] { sweepHeldKeys(); }};

    engine::AudioEngine& engine;
    engine::AudioDeviceService& audio;
    engine::TrackLoader& loader;
    engine::BufferRegistry& registry;
    std::array<juce::String, 2> deckStatus{"empty", "empty"};
    std::unique_ptr<juce::FileChooser> chooser;
    // Deck views (P1-13), left = A, right = B.
    ui::DeckView deckA{engine, 0, [this](engine::BufferId id) { return registry.get(id); }};
    ui::DeckView deckB{engine, 1, [this](engine::BufferId id) { return registry.get(id); }};
    ui::MixerView mixer{engine}; // P1-15, between the decks
    // Zoomed waveforms (P1-14): drag = scratch / scrub, wheel = nudge, Ctrl+wheel = zoom.
    double outputLatencySeconds() const;
    ui::WaveformView waveA{engine, 0, [this](engine::BufferId id) { return registry.get(id); },
                           [this] { return outputLatencySeconds(); }};
    ui::WaveformView waveB{engine, 1, [this](engine::BufferId id) { return registry.get(id); },
                           [this] { return outputLatencySeconds(); }};
    ui::DebugOverlay overlay;
    bool testToneOn = false;

public:
    void resetTempo(engine::DeckId deck);
    void setTempoRange(double rangePercent);
    void setHighQualityResampling(bool on);
    void toggleKeyLock(engine::DeckId deck);
    void setIsolator(bool on);
    void setCrossfaderCurve(int curve);
    void setCrossfaderAssign(int deck, int assign);
    void setCueMix(double mix);
    void setHeadphoneLevel(double db);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace devildj::app
