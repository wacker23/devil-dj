#pragma once

#include "SoakTest.h"
#include "engine/AudioDeviceService.h"
#include "engine/AudioEngine.h"
#include "engine/BufferRegistry.h"
#include "engine/TrackLoader.h"
#include "analysis/AnalysisService.h"
#include "library/Database.h"
#include "util/Beatgrid.h"
#include "util/MusicalKey.h"
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
    // Missing files (P2-10): check every track's file (also done at start, quietly), and relocate - ask for a folder
    // and search it by file name + size. Both run on the library worker thread.
    void checkMissingFiles(bool quiet = false);

    // Beatgrids (P2-14): the library's grid goes to a deck when its track loads; edits change the deck's grid at
    // once and are saved (an edited grid is locked, so analysis leaves it alone).
    enum class GridEdit
    {
        Downbeat,
        EarlierMs,
        LaterMs,
        BeatBack,
        BeatForward,
        Double,
        Halve,
        ToggleLock,
    };
    void editGrid(engine::DeckId deck, GridEdit edit);
    void tapTempo(engine::DeckId deck, double nowSeconds = -1.0);
    void toggleMetronome(engine::DeckId deck);
    util::Beatgrid deckGrid(engine::DeckId deck) const; // in the deck buffer's samples
    std::int64_t deckTrackId(engine::DeckId deck) const;
    void showGridMenu(engine::DeckId deck);
    // Reads the grid of the deck's track from the library again (after analysis or an edit elsewhere).
    void reloadGrid(engine::DeckId deck);
    analysis::AnalysisService* analysis() noexcept { return analysisService.get(); }
    void relocateMissingFiles(const std::vector<std::int64_t>& trackIds = {});

    // Asks for a library file from other software (DJ library XML / iTunes Library.xml) and imports it (P2-09).
    void importOtherLibrary();
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
    struct DeckGrid
    {
        std::int64_t trackId = 0;
        double fileRate = 0.0, bufferRate = 0.0;
        util::Beatgrid grid; // buffer samples
        util::TapTempo taps;
        bool metronome = false;
        int musicalKey = -1; // the library's key for the loaded track
    };
    util::KeyNotation keyNotation = util::KeyNotation::Musical;
    void showDeckKey(engine::DeckId deck);
    std::array<DeckGrid, 2> deckGrids;
    std::unique_ptr<library::Connection> gridReader; // message thread
    int gridListener = 0;
    std::shared_ptr<std::atomic<bool>> gridsChanged = std::make_shared<std::atomic<bool>>(false);
    void loadGridFor(engine::DeckId deck, const juce::File& file, double bufferRate);
    void saveGrid(engine::DeckId deck);
    void sendGrid(engine::DeckId deck);
    // Analysis (P2-12): imported and not-yet-analyzed tracks, in the background; progress in the browser.
    std::unique_ptr<analysis::AnalysisService> analysisService;
    std::unique_ptr<juce::TimedCallback> analysisProgress;
    void showAnalysisProgress();
    int analysisTotal = 0;
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
    void setKeyNotation(util::KeyNotation notation); // library Key column and the decks (P2-15)
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
