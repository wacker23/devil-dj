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
#include "ui/KeymapEditor.h"
#include "ui/NextTrackPanel.h"
#include "ui/BeatChannelStrip.h"
#include "ui/CodeChannelStrip.h"
#include "ui/CodeEditorPanel.h"
#include "ui/TransitionEditor.h"
#include "ui/BeatMakerPanel.h"
#include "app/BeatMakerProducer.h"
#include "app/CodeBuffers.h"
#include "app/CodeDeckProducer.h"
#include "app/BeatRender.h"
#include "app/ChoiceTracker.h"
#include "app/SmartCues.h"
#include "app/VocalPresence.h"
#include "library/PhraseRepository.h"
#include "app/DrumSwapBank.h"
#include "app/StemDeckLoader.h"
#include "app/StemService.h"
#include "app/SampleBank.h"
#include "app/TransitionStore.h"
#include "app/SpotifyAuth.h"
#include "app/SpotifyImportWindow.h"
#include "beatmaker/Model.h"
#include "beatmaker/Sequencer.h"
#include "ui/RecorderPanel.h"
#include "ui/SamplerPanel.h"
#include "engine/Recorder.h"
#include "Automix.h"
#include "MainLayout.h"
#include "matcher/MatcherService.h"
#include "PadFxPresets.h"
#include "ui/AutomixPanel.h"
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
    juce::TextButton fxTab{"FX"}, samplerTab{"SAMPLER"}, recordTab{"REC"}, automixTab{"AUTOMIX"};
    int shownPanel = 0; // 0 FX, 1 sampler, 2 recorder, 3 automix
    // Automix (P3-18): the controller, its panel (the fourth tab), its queue of files, F10.
    app::Automix automix;
    ui::AutomixPanel automixPanel;
    std::vector<juce::File> automixFiles;
    std::vector<juce::String> automixNames;
    std::vector<std::int64_t> automixIds; // the queue's library ids (smart mode scores them)
    int automixLogged = 0;                // how much of the played order is in the log
    int pickAutomixNext(int current, const std::vector<int>& remaining);
    std::array<int, 2> automixLoading{-1, -1}; // the queue item being loaded on each deck
    std::unique_ptr<juce::TimedCallback> automixTimer;
    void toggleAutomix();
    // Track Matcher (P3-19): the service on its worker thread, asked again on its triggers (MATCHER_SPEC §2); the
    // latest suggestions for the idle deck - N loads the highlighted one, Shift+N moves the highlight (the Next Track
    // panel showing them is P3-24).
    std::unique_ptr<matcher::MatcherService> matcherService;
    matcher::MatcherTriggers matcherTriggers;
    std::vector<matcher::Suggestion> suggestions;
    std::int64_t suggestionsFor = 0;
    int suggestionHighlight = 0;
    int analysisRuns = 0;
    std::unique_ptr<juce::TimedCallback> matcherTimer;
    void askMatcher(bool now = false); // now: whatever the triggers say (a load, a setting changed)
    void loadSuggestion();
    // The Next Track panel (P3-24) and the matcher's settings it shows.
    ui::NextTrackPanel nextTrackPanel;
    // The Beat Maker (P4-06): its project (saved to Documents\Devil-DJ\Beats\Current.ddjbeat a moment after each
    // edit) and the step grid, shown in the browser's place (Ctrl+Tab switches).
    beatmaker::Project beatProject;
    ui::BeatMakerPanel beatPanel;
    int bottomPanel = 0; // 0 the browser, 1 the Beat Maker, 2 the transition editor (P4-17), 3 the code deck (P5-12)
    // The transition editor (P4-17): the next transition, from the deck playing to the other one - Auto, then as
    // edited; the T key plays it while it is for the two tracks on the decks.
    ui::TransitionEditor mixEditor;
    struct MixPlan
    {
        std::int64_t fromTrack = 0, toTrack = 0;
        engine::BufferId fromBuffer = 0, toBuffer = 0;
        int from = 0;
        ui::TransitionEditor::Edit edit;
    };
    std::optional<MixPlan> mixPlan;
    std::shared_ptr<const engine::TrackBuffer> mixOutTrack, mixInTrack; // kept while the editor shows them
    void refreshMix(bool replan);
    void startPlannedTransition(const ui::TransitionEditor::Edit& edit, int from, bool isPreview);
    void loadMixPresetNames();
    // P4-19: saved transitions - the pair's saved one, else Auto (nullopt: not analyzed enough); saved as edited.
    std::optional<ui::TransitionEditor::Edit> transitionFor(int from, bool* saved = nullptr);
    app::TransitionLabels transitionLabels;
    std::unique_ptr<juce::TimedCallback> mixSaveTimer;
    std::unique_ptr<app::SpotifyAuth> spotify; // P4-20
    juce::String spotifyClientId;
    int spotifyPort = 43879;
    juce::Component::SafePointer<juce::DialogWindow> spotifyImportWindow;
    std::unique_ptr<juce::FileChooser> spotifyChooser;
    void saveMixPlan();
    bool beatPlaying = false;
    bool beatSongMode = false, beatSwitchAtEnd = false;
    // P4-10: the producer thread that plays the patterns into the engine, and the BEATS channel strip.
    std::unique_ptr<app::BeatMakerProducer> beatProducer;
    ui::BeatChannelStrip beatStrip{engine};
    // The code deck (P5-12): its producer (its own thread, the JS runtime), the sound names, its channel strip.
    std::shared_ptr<app::CodeSoundBank> codeSounds;
    std::unique_ptr<app::CodeDeckProducer> codeProducer;
    std::vector<std::pair<std::uint16_t, app::CodeSound>> codeSoundsToSend; // not yet taken by the engine
    bool codeWasPlaying = false;
    ui::CodeChannelStrip codeStrip{engine};
    // The code editor (P5-13): its tabs, kept in Documents\Devil-DJ\Code, saved a second after typing stops.
    ui::CodeEditorPanel codeEditor;
    app::CodeBuffers codeBuffers;
    std::unique_ptr<juce::TimedCallback> codeSaveTimer;
    void setUpCodeEditor();
    void saveCodeBuffers();
    // P5-14: Ctrl+Enter (at the next cycle line) / Ctrl+Shift+Enter (now) / Ctrl+. (hush).
    void evaluateCode(bool now);
    std::vector<juce::String> soundNamesForCompletion(); // P5-16: built-in names and sample folders
    // P5-17: the shown tab rendered offline (on a thread with a big stack for the JavaScript).
    void showCodeRenderMenu();
    void renderCode(int cycles, bool toSampler);
    std::unique_ptr<juce::Thread> codeRenderJob;
    std::unique_ptr<juce::FileChooser> codeRenderChooser;
    void hushCode();
    int codeEvaluatedTab = 0;               // the tab the running evaluation came from
    std::uint64_t codeEvaluationsSeen = 0;  // the producer's evaluations already shown
    std::size_t codeWarningsSeen = 0;
    std::uint64_t codePushedBefore = 0;     // events / s: the count and the time a second ago
    double codeRateTime = 0.0, codeEventsPerSecond = 0.0;
    void updateCodeDeck(); // the app's timer: clock, DJ hooks, new sounds, hush, the strip
    std::array<std::optional<beatmaker::SoundRef>, 128> sentDrumSounds;
    std::array<std::optional<beatmaker::SynthPreset>, 128> sentSynthSounds;
    // P4-13: user samples, decoded once and kept; what each drum sound index was last given.
    app::SampleBank sampleBank{samplesFolder()};
    std::array<std::optional<const engine::SampleData*>, 128> sentSamples;
    void setBeatSample(int track, const juce::File& file);
    void chooseBeatSample(int track);
    void startBeatMaker();
    void stopBeatMaker();
    void beatProjectChanged(); // the producer's copy and the voices' sounds follow the project
    void beatHit(int track);   // P4-11: heard now; recorded into the nearest step while REC is on and playing
    // P4-12: export the pattern shown / the song - to a WAV file, a sampler slot or the library.
    enum class BeatExport
    {
        PatternWav,
        Pattern4Wav,
        SongWav,
        PatternSampler,
        PatternLibrary,
        SongLibrary,
    };
    void showBeatExportMenu();
    void exportBeat(BeatExport what);
    static juce::File beatRendersFolder();
    std::unique_ptr<juce::FileChooser> beatExportChooser;
    std::unique_ptr<juce::TimedCallback> beatTimer, beatSaveTimer;
    void switchBottomPanel();
    static juce::File beatProjectFile();
    bool showNextTrack = true;
    int matcherIntent = 1, matcherKeyPolicy = 1, matcherScope = 0, matcherPreset = 0;
    std::vector<double> energyHistory;
    std::int64_t lastMasterTrack = 0;
    std::vector<matcher::Preset> matcherPresets = matcher::defaultPresets();
    // P6-11: the choice log - what was shown, what was chosen, how it went.
    ChoiceTracker choiceTracker;
    bool choiceLogging = true;
    std::int64_t choiceSession = 0, nextChoiceId = 1;
    std::optional<matcher::MatchRequest> suggestionsRequest; // what the shown suggestions answer
    std::unique_ptr<juce::FileChooser> choiceExportChooser;
    void logChoice(engine::DeckId deck); // a track loaded on `deck`
    void showSuggestions();
    void previewSuggestion(bool down);
    struct Preview
    {
        bool on = false;
        int deck = -1;
        float fader = 1.0f;
        bool cue = false;
    } preview;
    std::optional<double> energyOf(std::int64_t trackId);
    void queueAutomix();
    void applyAutomixSettings();
    void showPanel(int which);
    // Mix recording (P3-16): the recorder, its panel (the third tab), F9; a tracklist and a cue sheet per file.
    engine::Recorder recorder{engine.recordTap()};
    ui::RecorderPanel recorderPanel{engine, recorder};
    void toggleRecording();
    // Auto-transition (P3-17): T plans one from the playing deck to the other (T again / Shift+T cancels).
    void autoTransition();
    int transitionBeats = 16, transitionStyle = 0;

public:
    void setTransitionSettings(int beats, int style)
    {
        transitionBeats = beats;
        transitionStyle = style;
    }

private:
    void writeRecordingSheets(const engine::Recorder::FileInfo& file);
    static juce::File recordingsFolder();
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
    // Drum Swap (P6-05): the banks (loaded once the device rate is known), the pads, capture, Beat Maker loops.
    std::unique_ptr<DrumSwapBank> drumSwaps;
    std::unique_ptr<juce::TimedCallback> drumSwapLoader;
    void loadDrumSwaps();
    void putDrumSwap(engine::DeckId deck, int slot, DrumSwapSlot loop);
    void clearDrumSwap(engine::DeckId deck, int slot);
    void captureDrumSwap(engine::DeckId deck, int slot, int beats);
    void drumSwapFromBeat(engine::DeckId deck, int slot, int pattern);
    void showDrumSwapMenu(engine::DeckId deck, int slot);
    void showDrumSwapLooks();
    BeatRenderOptions beatRenderOptions(int pattern); // the Beat Maker's render settings (export, Drum Swap)
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
    app::WaveLayout waveLayout = app::WaveLayout::Horizontal; // P3-23
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
        library::StoredPhrases phrases; // P6-06 (seconds)
        std::optional<double> mixIn, mixOut; // P6-08 (buffer samples)
        std::shared_ptr<const VocalCurve> vocals; // P6-09 (from the stems)
    };
    // Vocal presence (P6-09): made from a track's vocal stem in the background - stored (vocal_intro / outro) and,
    // for a track on a deck, drawn per phrase on its waveform. At start, every separated track without them.
    void refreshVocals(std::vector<std::pair<std::int64_t, std::string>> tracks);
    void backfillVocals();
    // Mix points (P6-08): loaded with the track, set at a bar (keys, right-click the waveform), shown on the waveform,
    // used by Auto (the plan) and Automix.
    void loadMixPoints(engine::DeckId deck);
    void setMixPoint(engine::DeckId deck, bool out, std::optional<double> sample);
    void showMixPoints(engine::DeckId deck);
    // Smart cues (P6-07): suggestions on the empty pads A-H; the habits learnt from the library at start (and again
    // every few answers); the answers saved (cue_feedback) and the acceptance rate logged.
    CueHabits cueHabits;
    CueAnswers cueAnswers;
    int answersSinceTraining = 0;
    std::array<std::vector<CueCandidate>, 2> cueSuggestions;
    std::array<std::array<int, 8>, 2> suggestionPads{};
    std::array<std::vector<double>, 2> rejectedHere; // this session's rejections (the library's are read too)
    void trainCueHabits();
    void updateCueSuggestions(engine::DeckId deck);
    void answerSuggestion(engine::DeckId deck, int pad, bool accept);
    // One answer: accept puts it on `pad`; `refresh` recomputes the pads after it.
    void answerCandidate(engine::DeckId deck, const CueCandidate& c, int pad, bool accept, bool refresh);
    juce::String smartCueStatus() const;
    // Phrases (P6-06): shown on the deck's waveform; right-click there to edit (saved as the owner's).
    void showPhrases(engine::DeckId deck);
    void showPhraseMenu(engine::DeckId deck, double sample);
    void savePhrases(engine::DeckId deck, std::vector<library::StoredPhrase> phrases);
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
    // Stems (P6-03): right-click > Separate stems; FLACs in the cache, progress in the browser after analysis.
    std::unique_ptr<StemService> stemService;
    // Stem playback (P6-04): a deck's stems loaded from the cache when its track has them (or when a separation of
    // the track on it finishes).
    std::unique_ptr<StemDeckLoader> stemLoader;
    void loadStemsFor(engine::DeckId deck, const engine::TrackBuffer& track);
    void loadMissingStems(); // decks whose track has stems now but none loaded
    int stemsDone = 0, stemsFailed = 0;
    void separateStems(const std::vector<std::int64_t>& trackIds, bool quality);
    juce::String stemActivity();
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
    void showStemProgress();
    int analysisTotal = 0;
    std::unique_ptr<juce::FileChooser> importChooser;
    ui::DebugOverlay overlay;
    ui::KeymapOverlay keymapOverlay; // Ctrl+/ held (P1-17)
    juce::Component::SafePointer<ui::KeymapEditor> keymapEditor; // P3-20, in its own window while open
    std::unique_ptr<juce::FileChooser> keymapChooser;
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
    // P3-16: the recorder's options (from the settings), and their changes.
    void setRecorderSettings(const ui::RecorderPanel::Settings& s) { recorderPanel.setSettings(s); }
    std::function<void(const ui::RecorderPanel::Settings&)> onRecorderSettingsChanged;
    // P3-18: Automix's options (from the settings), and their changes.
    void setAutomixSettings(const ui::AutomixPanel::Settings& s)
    {
        automixPanel.setSettings(s);
        applyAutomixSettings();
    }
    std::function<void(const ui::AutomixPanel::Settings&)> onAutomixSettingsChanged;
    // P3-23: the layout (0 horizontal waveforms, 1 vertical) and the enlarged browser; changes reported to save.
    void setLayout(int waveforms, bool bigBrowser);
    void setNextTrackPanel(bool show); // P3-24
    // P4-20: Spotify sign-in (the Client ID and port from the settings).
    void setSpotifyApp(const juce::String& clientId, int redirectPort);
    void spotifyMenu(int op); // 0 the Client ID (asks), 1 sign in, 2 sign out, 3 import playlists (P4-21)
    // P6-11: the choice log - logging on / off (from the settings), 1 export CSV, 2 export JSON, 3 delete (asks).
    void setChoiceLogging(bool on) noexcept { choiceLogging = on; }
    void choiceLogMenu(int op);
    void showSpotifyImport();
    std::function<void(const juce::String& clientId)> onSpotifyClientIdChanged;
    std::function<void()> onSpotifyStateChanged; // signed in / out: the menu follows
    bool spotifySignedIn() const { return spotify != nullptr && spotify->signedIn(); }
    bool spotifyHasClientId() const { return spotifyClientId.isNotEmpty(); }
    std::function<void(bool show)> onNextTrackPanelChanged;
    std::function<void(int waveforms, bool bigBrowser)> onLayoutChanged;
    std::function<void(const juce::String& json)> onPadFxPresetsChanged;
    void setIsolator(bool on);
    void setCrossfaderCurve(int curve);
    void setCrossfaderAssign(int deck, int assign);
    void setCueMix(double mix);
    void setHeadphoneLevel(double db);

    // User keymap (P1-17): Documents\Devil-DJ\Keymaps\keymap.json on top of the KEYMAP.md defaults.
    static juce::File keymapFolder();
    void reloadKeymap();
    void editKeymap(); // P3-20: the keymap editor (Options > Edit keymap)
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
