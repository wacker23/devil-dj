#pragma once

#include "HotCueStore.h"
#include "LoopExport.h"
#include "MemoryCues.h"
#include "PlayTracker.h"
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
#include "ui/FxPanel.h"
#include "ui/SamplerPanel.h"
#include "PadFxPresets.h"
#include "SequenceStore.h"
#include "ui/LibraryBrowser.h"
#include "ui/MemoryCuePanel.h"
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
    static juce::File cacheFolder();   // %LOCALAPPDATA%\Devil-DJ\cache: <content hash>.wfm waveforms (P2-17)
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
    ui::FxPanel fxPanel{engine}; // P3-09, between the waveforms and the browser
    // The sampler (P3-13) in the same place: FX / SAMPLER tabs.
    ui::SamplerPanel samplerPanel{engine};
    juce::TextButton fxTab{"FX"}, samplerTab{"SAMPLER"};
    bool showSampler = false;
    void showPanel(bool sampler);
    void loadSample(int slot, const juce::File& file, double bpm, const juce::String& name);
    // The sequencer's sequences (P3-15): the store, which one is in the engine, saving it when it changes.
    app::SequenceStore sequences;
    int sequenceBank = 0, sequenceIndex = 0;
    void callSequence(int bank, int index, bool play);
    void keepSequence(); // the engine's sequence into the store (and the settings) when it has changed
    void showSequenceLooks();
    // P3-14: the deck's loop (or, with slicer = true, its slicer range) into a sampler slot - the shown bank's first
    // empty one - and saved as a 32-bit float WAV in Documents\Devil-DJ\Samples.
    void captureToSampler(engine::DeckId deck, bool slicer);
    static juce::File samplesFolder();
    void chooseSampleFile(int slot);
    std::unique_ptr<juce::FileChooser> sampleChooser;
    void chooseEffect(int unit, int slot, int type); // creates and installs it (-1: empty the slot)
    std::unique_ptr<juce::TimedCallback> fxTimer;     // hands replaced effects back for deleting
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
        std::optional<double> loudnessLufs;
        bool autoGainPending = false; // loaded before its loudness was known: set the trim when it is
        bool autoGainAtLoad = false;  // the first time, right after the load
        float autoGainTrim = 0.0f;    // the trim Auto Gain set (a later analysis result only replaces that)
    };
    bool autoGain = true;
    double autoGainTarget = -10.0;
    bool quantize = false;
    double quantizeBeats = 1.0;
    std::array<int, 2> jumpSizeIndex{kDefaultJumpSize, kDefaultJumpSize};
    bool keySyncCompatible = false;
    void applyAutoGain(engine::DeckId deck, bool atLoad);
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
    // History (P2-23): plays over 60 s go into this run's session (created with its first play).
    PlayTracker playTracker;
    std::unique_ptr<juce::TimedCallback> historyTimer;
    double historyLastTick = 0.0;
    std::shared_ptr<std::atomic<std::int64_t>> historySession = std::make_shared<std::atomic<std::int64_t>>(0);
    void trackHistory();
    // Hot cues (P3-01): the loaded tracks' cues as the library has them; saved when the engine's change.
    std::array<HotCues, 2> hotCues{};
    std::array<bool, 2> hotCuesInEngine{}; // the engine shows this track's cues (not the last track's) yet
    int hotCuePage = 0;                    // 0 = pads A-H, 1 = I-P
    void loadHotCues(engine::DeckId deck);
    void syncHotCues();
    void saveHotCues(engine::DeckId deck, std::vector<HotCueChange> changes);
    void showHotCueLooks(engine::DeckId deck);
    void showHotCueMenu(engine::DeckId deck, int pad);
    void setHotCuePage(int page);
    // The pad grid (P3-06): each deck's mode; a pad press / release in it; the mode menu.
    std::array<ui::PadMode, 2> padModes{ui::PadMode::HotCue, ui::PadMode::HotCue};
    std::array<double, 2> slicerBeats{8.0, 8.0}; // P3-07
    // Pad FX (P3-12): the presets, the bank on the pads, its effects installed behind both decks' pads.
    app::PadFxPresets padFxPresets = app::defaultPadFxPresets();
    int padFxBank = 0;
    void installPadFxBank();
    void showPadFxMenu(engine::DeckId deck, int pad);
    void editPadFx(int pad, const app::PadFxPreset& preset); // the current bank's pad
    void setPadMode(engine::DeckId deck, ui::PadMode mode);
    void padAction(engine::DeckId deck, int pad, bool alternate, bool release);
    void showPadModeMenu(engine::DeckId deck);
    // Memory cues and loops (P3-02).
    std::array<std::vector<MemoryCue>, 2> memoryCues;
    std::array<juce::Component::SafePointer<ui::MemoryCuePanel>, 2> memoryPanels;
    void loadMemoryCues(engine::DeckId deck);
    void showMemoryCues(engine::DeckId deck); // markers and the open list
    void addMemoryCue(engine::DeckId deck);
    void callMemoryCue(engine::DeckId deck, const MemoryCue& cue);
    void callAdjacentMemoryCue(engine::DeckId deck, int direction);
    void openMemoryPanel(engine::DeckId deck);
    void editMemoryCue(engine::DeckId deck, std::function<void(library::Connection&)> change);
    // Loops (P3-03).
    void loopButton(engine::DeckId deck, ui::DeckView::LoopButton button);
    void showLoopMenu(engine::DeckId deck);
    void exportLoop(engine::DeckId deck);
    std::unique_ptr<juce::FileChooser> loopChooser;
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
    void setAutoGain(bool on, double targetLufs);    // P2-16: applies to the next loads
    // Quantize (P2-20): on / off and the resolution in beats; the ` key toggles it (onQuantizeChanged tells the
    // app, to save it and tick the menu).
    void setQuantize(bool on, double beats);
    // Beat jump (P2-21): each deck's jump size, an index into kJumpSizes.
    static constexpr double kJumpSizes[] = {0.125, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128};
    static constexpr int kDefaultJumpSize = 5; // 4 beats
    static juce::String jumpSizeText(double beats); // "1/8", "1/2", "4", ...
    void beatJump(engine::DeckId deck, int direction);
    // Key Sync / Key Shift (P2-22): both turn key lock on (a shift is only heard with it).
    void keySync(engine::DeckId deck);
    void shiftKey(engine::DeckId deck, int semitones);
    void setKeySyncCompatible(bool on) noexcept { keySyncCompatible = on; }
    void stepJumpSize(engine::DeckId deck, int steps);
    void showJumpSizeMenu(engine::DeckId deck);
    double jumpSize(engine::DeckId deck) const noexcept;
    std::function<void(bool on, double beats)> onQuantizeChanged;
    void toggleKeyLock(engine::DeckId deck);
    // P3-05: how PAUSE stops (0 instantly, 1 vinyl brake, 2 spin-back) and the stop times in seconds.
    void setStopStyle(int style, double brakeSeconds, double spinBackSeconds);
    // P3-10: what the FILTER knobs do (the mixer's choice is reported through onColorFxChanged, for the settings).
    void setColorFx(int type);
    std::function<void(int type)> onColorFxChanged;
    // P3-12: the Pad FX presets as JSON (from the settings), and their changes (to save).
    void setPadFxPresets(const juce::String& json);
    // P3-15: the sequences as JSON (from the settings), and their changes (to save).
    void setSequences(const juce::String& json);
    std::function<void(const juce::String& json)> onSequencesChanged;
    std::function<void(const juce::String& json)> onPadFxPresetsChanged;
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
