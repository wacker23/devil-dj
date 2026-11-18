#pragma once

#include "engine/AudioDeviceService.h"
#include "engine/AudioEngine.h"
#include "engine/BufferRegistry.h"
#include "engine/TrackLoader.h"
#include "ui/DebugOverlay.h"
#include "ui/DeckView.h"
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

    // Temporary mixer keys (KEYMAP) until the mixer view (P1-15) and KeyboardController (P1-16).
    bool handleMixerKey(const juce::KeyPress& key);
    // Keys held: Shift+3/4, Shift+9/0 move the filter at half the knob range per second; G / H glide the
    // crossfader across in 1 s.
    void sweepHeldKeys();
    void sendCrossfader();
    double crossfader = 0.5;
    void sendFilter(int channel);
    std::array<std::array<bool, 3>, 2> kills{};
    std::array<double, 2> filterKnob{0.5, 0.5};
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
    // Zoomed waveforms (P1-14): drag = scratch / scrub, wheel = nudge, Ctrl+wheel = zoom.
    double outputLatencySeconds() const;
    ui::WaveformView waveA{engine, 0, [this](engine::BufferId id) { return registry.get(id); },
                           [this] { return outputLatencySeconds(); }};
    ui::WaveformView waveB{engine, 1, [this](engine::BufferId id) { return registry.get(id); },
                           [this] { return outputLatencySeconds(); }};
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
    void setIsolator(bool on);
    void setCrossfaderCurve(int curve);
    void setCrossfaderAssign(int deck, int assign);
    void setCueMix(double mix);
    void setHeadphoneLevel(double db);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace devildj::app
