#pragma once

#include "SoakTest.h"
#include "engine/AudioDeviceService.h"
#include "engine/AudioEngine.h"
#include "engine/BufferRegistry.h"
#include "engine/TrackLoader.h"
#include "library/Database.h"
#include "ui/Actions.h"
#include "ui/DebugOverlay.h"
#include "ui/DeckView.h"
#include "ui/LibraryBrowser.h"
#include "ui/KeyboardController.h"
#include "ui/KeymapOverlay.h"
#include "ui/MixerView.h"
#include "ui/WaveformView.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>

namespace devildj::app
{

// Root component of the main window: deck A | mixer | deck B on top, the zoomed waveforms, the library browser
// at the bottom. Hosts the debug overlay (F12). It is the drag-and-drop container for library rows dragged onto
// the decks.
class MainComponent final : public juce::Component, public juce::DragAndDropContainer
{
public:
    // libraryDb may be null when the library couldn't be opened (libraryProblem says why).
    MainComponent(engine::AudioEngine& engine, engine::AudioDeviceService& audio, engine::TrackLoader& loader,
                  engine::BufferRegistry& registry, library::Database* libraryDb = nullptr,
                  const juce::String& libraryProblem = {});
    ~MainComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void toggleDebugOverlay();
    void toggleTestTone();

    // Loading: from the browser panel, a dropped file, or (no file selected) a file chooser.
    void chooseTrackForDeck(engine::DeckId deck);
    void loadFile(engine::DeckId deck, const juce::File& file);
    ui::LibraryBrowser& libraryView() noexcept { return *libraryBrowser; }
    ui::FileBrowserPanel& browser() noexcept { return libraryBrowser->folders(); }

    // Library import (P2-04): files / folders on a worker thread; empty = ask for a folder. Progress and the
    // result show in the browser's status line.
    void importToLibrary(const juce::Array<juce::File>& filesOrFolders,
                         std::optional<std::int64_t> playlistParent = std::nullopt);
    // Asks for an .m3u / .m3u8 file and imports it as a playlist (P2-06).
    void importPlaylistFile(std::optional<std::int64_t> parentFolder = std::nullopt);
    bool isImporting() const;
    // Ctrl+B / the browser's BIGGER button: the browser takes the waveforms' room too (they hide), and back.
    void toggleBrowserSize();
    bool isBrowserBig() const noexcept { return browserBig; }
    static juce::File artworkFolder(); // %LOCALAPPDATA%\Devil-DJ\cache\artwork
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
    library::Database* libraryDb;
    std::unique_ptr<ui::LibraryBrowser> libraryBrowser; // P2-04, bottom
    std::unique_ptr<juce::Thread> importJob;
    bool browserBig = false;
    std::unique_ptr<juce::FileChooser> importChooser;
    ui::DebugOverlay overlay;
    ui::KeymapOverlay keymapOverlay; // Ctrl+/ held (P1-17)
    SoakTest soak;
    bool quitAfterSoak = false;
    std::unique_ptr<juce::TimedCallback> soakWaiter;
    void soakFinished(const juce::String& report);
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

    // User keymap (P1-17): Documents\Devil-DJ\Keymaps\keymap.json on top of the KEYMAP.md defaults.
    static juce::File keymapFolder();
    void reloadKeymap();
    void openKeymapFolder();

    // Soak test (P1-20): Options menu, or the command line (--soak=MIN [--soak-silent] [--soak-quit]
    // [--soak-tracks="a.mp3|b.mp3"]). Reports go to Documents\Devil-DJ\Soak.
    void startSoak(double minutes, bool silent);
    void stopSoak();
    // Waits (up to 2 minutes) until decks A and B have tracks, then starts; `quitWhenDone` closes the app after.
    void startSoakWhenLoaded(double minutes, bool silent, bool quitWhenDone);
    static juce::File soakFolder();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace devildj::app
