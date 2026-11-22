#include "MainComponent.h"

#include "analysis/Loudness.h"
#include "library/HistoryRepository.h"
#include "ui/TrackColours.h"

#include "engine/TrackDecoder.h"
#include "library/Database.h"
#include "library/ForeignLibrary.h"
#include "library/Importer.h"
#include "library/BeatgridRepository.h"
#include "library/MissingFiles.h"
#include "library/PlaylistFiles.h"
#include "util/Log.h"
#include "util/ProcessStats.h"

#include <taglib/tversionnumber.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

namespace devildj::app
{

namespace
{
constexpr int kOverlayMargin = 12;

std::filesystem::path toPath(const juce::File& f)
{
    return std::filesystem::path(f.getFullPathName().toWideCharPointer());
}

// A library job (missing-file check, relocate) on its own low-priority thread; its result goes to the browser's
// status line.
class LibraryTask final : public juce::Thread
{
public:
    using Work = std::function<juce::String(LibraryTask&)>;
    LibraryTask(juce::String name, Work w, ui::LibraryBrowser& b)
        : juce::Thread(std::move(name))
        , work(std::move(w))
        , browser(&b)
    {
    }
    ~LibraryTask() override { stopThread(10000); }
    void run() override { post(work(*this)); }
    bool cancelled() { return threadShouldExit(); }
    void post(const juce::String& text)
    {
        if (text.isEmpty())
            return;
        juce::MessageManager::callAsync(
            [b = browser, text]
            {
                if (b != nullptr)
                    b->setStatus(text);
            });
    }

private:
    Work work;
    juce::Component::SafePointer<ui::LibraryBrowser> browser;
};

// Runs library::importTracks on its own low-priority thread; progress and the result go to the browser's status
// line on the message thread.
class ImportJob final : public juce::Thread
{
public:
    ImportJob(library::Database& d, juce::Array<juce::File> in, juce::File art, ui::LibraryBrowser& b,
              std::optional<std::int64_t> parent, analysis::AnalysisService* a)
        : juce::Thread("Library import")
        , db(d)
        , inputs(std::move(in))
        , artwork(std::move(art))
        , browser(&b)
        , playlistParent(parent)
        , analyzer(a)
    {
    }
    ~ImportJob() override { stopThread(10000); }

    void run() override
    {
        std::vector<std::filesystem::path> paths, playlists, libraries;
        for (const auto& f : inputs)
            (f.hasFileExtension(".xml")              ? libraries
             : library::isPlaylistFile(toPath(f)) ? playlists
                                                  : paths)
                .push_back(toPath(f));
        library::ImportOptions options;
        options.artworkCacheDir = toPath(artwork);
        options.isCancelled = [this] { return threadShouldExit(); };
        if (analyzer != nullptr) // new tracks go straight to the analysis queue (P2-12)
            options.queueAnalysis = [a = analyzer](std::int64_t id) { a->enqueue(id); };
        options.onProgress = [this](int done, int total)
        { post("Adding to the library... " + juce::String(done) + " / " + juce::String(total)); };
        // Libraries from other software (P2-09): tracks, cues, tempo markers and playlists.
        juce::String playlistText;
        for (const auto& file : libraries)
        {
            post("Reading " + juce::String(file.filename().wstring().c_str()) + "...");
            const auto foreign = library::readForeignLibrary(file);
            if (!foreign)
            {
                playlistText << "Library import failed: " << juce::String::fromUTF8(foreign.error().c_str()) << ". ";
                continue;
            }
            post("Adding " + juce::String(static_cast<int>(foreign.value().tracks.size())) + " tracks from " +
                 juce::String::fromUTF8(foreign.value().format.c_str()) + "...");
            const auto folder = "Imported " + foreign.value().format + " " +
                                juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M").toStdString();
            const auto applied = library::importForeignLibrary(db, foreign.value(), folder, options);
            if (!applied)
            {
                playlistText << "Library import failed: " << juce::String::fromUTF8(applied.error().c_str()) << ". ";
                continue;
            }
            const auto& a = applied.value();
            playlistText << "From " << juce::String::fromUTF8(foreign.value().format.c_str()) << ": " << a.matched
                         << " of " << a.tracksInFile << " tracks (" << a.added << " new), " << a.playlists
                         << " playlists, " << a.cuesAdded << " cues";
            if (a.missingFiles > 0)
                playlistText << ", " << a.missingFiles << " files not on this computer";
            playlistText << ". ";
            util::logInfo("Library import ({}): {} tracks in the file, {} matched, {} new, {} missing, {} cues, {} "
                          "beatgrids, {} playlists",
                          foreign.value().format, a.tracksInFile, a.matched, a.added, a.missingFiles, a.cuesAdded,
                          a.beatgridsAdded, a.playlists);
            for (const auto& n : a.notes)
                util::logWarning("Library import: {}", n);
        }

        // Playlist files (P2-06): each becomes a playlist; tracks not in the library yet are added first.
        for (const auto& p : playlists)
        {
            post("Importing playlist " + juce::String(p.filename().wstring().c_str()) + "...");
            const auto made = library::importPlaylist(db, p, playlistParent, options);
            if (!made)
            {
                playlistText << "Playlist failed: " << juce::String::fromUTF8(made.error().c_str()) << ". ";
                continue;
            }
            const auto& m = made.value();
            playlistText << "Playlist \"" << juce::String::fromUTF8(m.name.c_str()) << "\": " << m.tracks
                         << " tracks";
            if (m.addedToLibrary > 0)
                playlistText << " (" << m.addedToLibrary << " new in the library)";
            if (!m.missing.empty())
                playlistText << ", " << static_cast<int>(m.missing.size()) << " missing";
            playlistText << ". ";
            for (const auto& missing : m.missing)
                util::logWarning("Playlist import {}: missing {}", juce::String(p.wstring().c_str()).toStdString(), missing);
        }
        if (paths.empty())
        {
            post(playlistText.trim());
            return;
        }

        post("Looking for music...");
        const auto r = library::importTracks(db, paths, options);

        juce::String text = r.cancelled ? "Import stopped: " : "Added ";
        text << r.added << (r.added == 1 ? " track" : " tracks");
        if (r.alreadyInLibrary > 0)
            text << ", " << r.alreadyInLibrary << " already in the library";
        if (r.duplicates > 0)
            text << ", " << r.duplicates << " duplicate" << (r.duplicates == 1 ? "" : "s");
        if (r.failed > 0)
            text << ", " << r.failed << " unreadable";
        text << " (" << juce::String(r.seconds, 1) << " s)";
        if (r.relocated > 0)
            text << ", " << r.relocated << " moved file" << (r.relocated == 1 ? "" : "s") << " found again";
        if (r.found == 0)
            text = "No music files found there";
        util::logInfo("Library import: {} found, {} added, {} known, {} duplicates, {} failed, {:.1f} s", r.found,
                      r.added, r.alreadyInLibrary, r.duplicates, r.failed, r.seconds);
        for (const auto& e : r.errors)
            util::logWarning("Library import: {}", e);
        post(playlistText + text);
    }

private:
    void post(const juce::String& text)
    {
        juce::MessageManager::callAsync(
            [b = browser, text]
            {
                if (b != nullptr)
                    b->setStatus(text);
            });
    }

    library::Database& db;
    juce::Array<juce::File> inputs;
    juce::File artwork;
    juce::Component::SafePointer<ui::LibraryBrowser> browser;
    std::optional<std::int64_t> playlistParent;
    analysis::AnalysisService* analyzer = nullptr;
};

// The analysis service's decoder: the engine's (Media Foundation / JUCE readers) at the file's own rate.
util::Result<analysis::DecodedAudio> decodeForAnalysis(const std::filesystem::path& file, const std::atomic<bool>& cancel)
{
    using R = util::Result<analysis::DecodedAudio>;
    engine::DecodeOptions options;
    options.cancel = &cancel;
    auto decoded = engine::decodeTrack(juce::File(juce::String(file.wstring().c_str())), options);
    if (!decoded)
        return R::fail(decoded.error());
    std::shared_ptr<engine::TrackBuffer> buffer = std::move(decoded).value();
    analysis::DecodedAudio a;
    a.left = buffer->audio.getReadPointer(0);
    a.right = buffer->audio.getNumChannels() > 1 ? buffer->audio.getReadPointer(1) : a.left;
    a.numSamples = buffer->audio.getNumSamples();
    a.sampleRate = buffer->sampleRate;
    a.owner = buffer;
    return R::ok(std::move(a));
}
} // namespace

MainComponent::MainComponent(engine::AudioEngine& audioEngine, engine::AudioDeviceService& audioService,
                             engine::TrackLoader& trackLoader, engine::BufferRegistry& bufferRegistry,
                             library::Database* db, const juce::String& libraryProblem)
    : engine(audioEngine)
    , audio(audioService)
    , loader(trackLoader)
    , registry(bufferRegistry)
    , soak(SoakTest::Hooks{[&audioEngine] { return audioEngine.readState(); }, [&audioService]
                           { return audioService.getStatus(); }, [&audioEngine](const engine::EngineCommand& c)
                           { audioEngine.send(c); }, [] { return util::processMemory().privateMb; },
                           [] { return juce::Time::getMillisecondCounterHiRes() / 1000.0; }})
    , libraryDb(db)
    , libraryBrowser(std::make_unique<ui::LibraryBrowser>(db, artworkFolder(), libraryProblem))
    , overlay([&audioEngine] { return audioEngine.readState(); }, [&audioService] { return audioService.getStatus(); })
{
    addAndMakeVisible(deckA);
    addAndMakeVisible(deckB);
    addAndMakeVisible(mixer);
    addAndMakeVisible(waveA);
    addAndMakeVisible(waveB);
    addAndMakeVisible(*libraryBrowser);
    libraryBrowser->onLoad = [this](int deck, const juce::File& file)
    { loadFile(static_cast<engine::DeckId>(deck), file); };
    libraryBrowser->onImport = [this](const juce::Array<juce::File>& files) { importToLibrary(files); };
    libraryBrowser->onToggleSize = [this] { toggleBrowserSize(); };
    libraryBrowser->onSearchDone = [this] { grabKeyboardFocus(); }; // the decks' keys work again
    libraryBrowser->onImportPlaylist = [this](std::optional<std::int64_t> parent) { importPlaylistFile(parent); };
    libraryBrowser->onCheckMissing = [this] { checkMissingFiles(false); };
    deckA.onGridMenu = [this] { showGridMenu(0); };
    deckB.onGridMenu = [this] { showGridMenu(1); };
    deckA.onBeatJump = [this](int direction) { beatJump(0, direction); };
    for (engine::DeckId d = 0; d < 2; ++d)
    {
        auto& view = d == 0 ? deckA : deckB;
        view.onHotCuePad = [this, d](int pad, bool down)
        { engine.send(hotCueForPad(d, pad, hotCuePage, false, !down)); };
        view.onHotCueMenu = [this, d](int pad) { showHotCueMenu(d, pad); };
        view.onMemoryPanel = [this, d] { openMemoryPanel(d); };
        view.onLoopButton = [this, d](ui::DeckView::LoopButton b) { loopButton(d, b); };
    }
    deckB.onBeatJump = [this](int direction) { beatJump(1, direction); };
    deckA.onJumpSizeMenu = [this] { showJumpSizeMenu(0); };
    deckB.onJumpSizeMenu = [this] { showJumpSizeMenu(1); };
    deckA.onTap = [this] { tapTempo(0); };
    deckB.onTap = [this] { tapTempo(1); };
    waveA.setGridSource([this] { return deckGrid(0); });
    waveB.setGridSource([this] { return deckGrid(1); });
    if (libraryDb != nullptr)
    {
        if (auto r = libraryDb->openReader())
            gridReader = std::move(r).value();
        // Analysis results (or other edits) for a loaded track: read its grid again on the message thread.
        gridListener = libraryDb->addChangeListener(
            [flag = gridsChanged, safe = juce::Component::SafePointer<MainComponent>(this)](
                const std::vector<library::Change>& changes)
            {
                bool tracks = false;
                for (const auto& c : changes)
                    tracks |= c.table == library::Table::Tracks;
                if (!tracks || flag->exchange(true))
                    return;
                juce::MessageManager::callAsync(
                    [flag, safe]
                    {
                        flag->store(false);
                        if (safe != nullptr)
                            for (engine::DeckId d = 0; d < 2; ++d)
                                safe->reloadGrid(d);
                    });
            });
    }
    if (libraryDb != nullptr)
    {
        analysis::AnalysisService::Options analysisOptions;
        analysisOptions.waveformCache = cacheFolder().getFullPathName().toWideCharPointer();
        analysisService = std::make_unique<analysis::AnalysisService>(*libraryDb, &decodeForAnalysis, analysisOptions);
        analysisTotal = analysisService->queueOutdated();
        if (analysisTotal > 0)
            util::logInfo("Analysis: {} tracks to analyze", analysisTotal);
        analysisProgress = std::make_unique<juce::TimedCallback>([this] { showAnalysisProgress(); });
        historyTimer = std::make_unique<juce::TimedCallback>([this] { trackHistory(); });
        historyTimer->startTimerHz(4);
        analysisProgress->startTimer(500);
    }
    libraryBrowser->onRelocate = [this](const std::vector<std::int64_t>& ids) { relocateMissingFiles(ids); };
    if (libraryDb != nullptr)
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MainComponent>(this)]
                                        {
                                            if (safe != nullptr)
                                                safe->checkMissingFiles(true); // at start, in the background
                                        });
    for (auto [target, deck] :
         {std::pair<ui::TrackDropTarget*, engine::DeckId>{&deckA, 0}, {&deckB, 1}, {&waveA, 0}, {&waveB, 1}})
        target->onTrackDropped = [this, deck = deck](const juce::File& file) { loadFile(deck, file); };
    overlay.setExtraLines([this] { return statusLines(); });
    addChildComponent(overlay);       // on top of the decks and waveforms
    addChildComponent(keymapOverlay); // above everything while Ctrl+/ is held

    loader.callbacks.onProgress = [this](engine::DeckId deck, const juce::File& file, double fraction)
    {
        if (deck < deckStatus.size())
            deckStatus[deck] =
                "loading " + file.getFileName() + " " + juce::String(juce::roundToInt(100 * fraction)) + " %";
    };
    loader.callbacks.onLoaded = [this](engine::DeckId deck, const engine::TrackBuffer& t)
    {
        loadGridFor(deck, t.file, t.sampleRate); // P2-14
        if (deck < deckStatus.size())
            deckStatus[deck] =
                t.file.getFileName() + " (" + juce::String(t.durationSeconds(), 1) + " s, " + t.formatName + ")";
    };
    loader.callbacks.onWaveformPreview = [this](engine::DeckId deck, const juce::File& file,
                                                std::shared_ptr<const analysis::Waveform> waveform, double)
    {
        if (deck > 1)
            return;
        (deck == 0 ? waveA : waveB).showPreview(waveform);
        (deck == 0 ? deckA : deckB).showPreview(waveform, file.getFileNameWithoutExtension());
    };
    loader.callbacks.onError = [this](engine::DeckId deck, const juce::File& file, const juce::String& error)
    {
        if (deck <= 1)
        {
            (deck == 0 ? waveA : waveB).clearPreview();
            (deck == 0 ? deckA : deckB).clearPreview();
        }
        if (deck < deckStatus.size())
            deckStatus[deck] = "failed: " + file.getFileName() + " - " + error;
    };
    soak.onFinished = [this](const juce::String& report) { soakFinished(report); };
    registerActions();
    reloadKeymap();
    keyHook = std::make_unique<ui::PhysicalKeyHook>([this](const ui::KeyboardController::KeyEvent& e)
                                                    { return onPhysicalKey(e); });
    if (!keyHook->isInstalled())
        util::logWarning("Keyboard hook not installed: performance keys are unavailable");
    setWantsKeyboardFocus(true);
    // Default size (a saved window position wins): up to 1440 x 900, never bigger than the screen.
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    const auto screen = display != nullptr ? display->userBounds.toNearestInt() : juce::Rectangle<int>(0, 0, 1280, 800);
    setSize(std::min(1440, screen.getWidth() - 40), std::min(900, screen.getHeight() - 60));
}

// ---- keys (P1-16): physical key -> KeyboardController -> ActionDispatcher -> handlers below ----

bool MainComponent::onPhysicalKey(const ui::KeyboardController::KeyEvent& e)
{
    // Only while the main window has the keyboard (dialogs and file choosers get their keys untouched).
    auto* peer = getPeer();
    if (peer == nullptr || !peer->isFocused())
    {
        keyboard.releaseAll();
        return false;
    }
    const auto* focused = juce::Component::getCurrentlyFocusedComponent();
    const bool typing = dynamic_cast<const juce::TextInputTarget*>(focused) != nullptr;
    return keyboard.handle(e, ui::KeyContext::Main, typing);
}

void MainComponent::registerActions()
{
    using DA = ui::DeckAction;
    using GA = ui::GlobalAction;
    using Phase = ui::ActionPhase;
    const auto onPress = [this](ui::CommandId id, std::function<void()> f)
    {
        actions.setHandler(id,
                           [f = std::move(f)](Phase p)
                           {
                               if (p == Phase::Press)
                                   f();
                           });
    };

    for (engine::DeckId d = 0; d < 2; ++d)
    {
        const auto cmd = [d](DA a) { return ui::deckCommand(d, a); };
        actions.setHandler(cmd(DA::Cue),
                           [this, d](Phase p)
                           {
                               if (p == Phase::Press)
                                   engine.send(engine::CuePress{d});
                               else if (p == Phase::Release)
                                   engine.send(engine::CueRelease{d});
                           });
        onPress(cmd(DA::Play), [this, d] { engine.send(engine::TogglePlay{d}); });
        for (const auto [action, sign] : {std::pair{DA::BendDown, -1.0}, std::pair{DA::BendUp, 1.0}})
            actions.setHandler(cmd(action),
                               [this, d, sign = sign](Phase p)
                               {
                                   if (p == Phase::Press)
                                       engine.send(engine::PitchBend{d, sign * engine::Deck::kMaxPitchBendPercent});
                                   else if (p == Phase::Release)
                                       engine.send(engine::PitchBend{d, 0.0});
                               });
        for (const auto [action, step] : {std::pair{DA::TempoDown, -0.1}, std::pair{DA::TempoUp, 0.1}})
            actions.setHandler(cmd(action),
                               [this, d, step = step](Phase p)
                               {
                                   if (p != Phase::Release)
                                       engine.send(engine::NudgeTempo{d, step});
                               });
        actions.setHandler(cmd(DA::FilterToLowPass), [](Phase) {}); // swept every frame while held
        actions.setHandler(cmd(DA::FilterToHighPass), [](Phase) {});
        onPress(cmd(DA::FilterReset),
                [this, d]
                {
                    filterKnob[d] = 0.5;
                    sendFilter(d);
                });
        onPress(cmd(DA::JumpToStart), [this, d] { engine.send(engine::Seek{d, 0.0}); });
        for (int band = 0; band < 3; ++band)
            onPress(cmd(static_cast<DA>(static_cast<int>(DA::KillLow) + band)),
                    [this, d, band]
                    {
                        // Toggle from what the engine has (the mixer knobs may have moved it).
                        const bool kill =
                            !std::isinf(engine.readState().channels[d].eqDb[static_cast<std::size_t>(band)]);
                        const auto param =
                            static_cast<engine::ChannelParam>(static_cast<int>(engine::ChannelParam::EqLow) + band);
                        engine.send(engine::SetParam{engine::channelParam(d, param), kill ? -1000.0f : 0.0f});
                    });
        onPress(cmd(DA::KeyLock), [this, d] { toggleKeyLock(d); });
        onPress(cmd(DA::Slip), [this, d] { engine.send(engine::SetSlip{d, !engine.readState().decks[d].slip}); });
        onPress(cmd(DA::Sync), [this, d] { engine.send(engine::SetSync{d, !engine.readState().decks[d].synced}); });
        // Hot cue pads (P3-01): pad 1-8 on the current page, Shift + pad deletes.
        for (int pad = 0; pad < 8; ++pad)
        {
            actions.setHandler(cmd(static_cast<DA>(static_cast<int>(DA::Pad1) + pad)),
                               [this, d, pad](Phase p)
                               {
                                   if (p != Phase::Repeat)
                                       engine.send(hotCueForPad(d, pad, hotCuePage, false, p == Phase::Release));
                               });
            onPress(cmd(static_cast<DA>(static_cast<int>(DA::PadAlt1) + pad)),
                    [this, d, pad] { engine.send(hotCueForPad(d, pad, hotCuePage, true, false)); });
        }
        // Beat jump (P2-21).
        onPress(cmd(DA::KeySync), [this, d] { keySync(d); }); // P2-22
        onPress(cmd(DA::MemoryAdd), [this, d] { addMemoryCue(d); }); // P3-02
        // Loops (P3-03).
        onPress(cmd(DA::AutoLoop), [this, d] { engine.send(engine::Loop{d, engine::LoopOp::Auto, 0.0}); });
        onPress(cmd(DA::LoopIn), [this, d] { engine.send(engine::Loop{d, engine::LoopOp::In}); });
        onPress(cmd(DA::LoopOut), [this, d] { engine.send(engine::Loop{d, engine::LoopOp::Out}); });
        // Reverse while held; brake / spin-back stop a playing deck (P3-05).
        actions.setHandler(cmd(DA::Reverse),
                           [this, d](Phase p)
                           {
                               if (p == Phase::Press)
                                   engine.send(engine::SetReverse{d, true});
                               else if (p == Phase::Release)
                                   engine.send(engine::SetReverse{d, false});
                           });
        onPress(cmd(DA::Brake), [this, d] { engine.send(engine::Stop{d, engine::StopKind::Brake}); });
        onPress(cmd(DA::SpinBack), [this, d] { engine.send(engine::Stop{d, engine::StopKind::SpinBack}); });
        onPress(cmd(DA::LoopHalve), [this, d] { engine.send(engine::Loop{d, engine::LoopOp::Halve}); });
        onPress(cmd(DA::LoopDouble), [this, d] { engine.send(engine::Loop{d, engine::LoopOp::Double}); });
        onPress(cmd(DA::Reloop),
                [this, d]
                {
                    const bool looping = engine.readState().decks[d].loopActive;
                    engine.send(engine::Loop{d, looping ? engine::LoopOp::Exit : engine::LoopOp::Reloop});
                });
        onPress(cmd(DA::CallPrevious), [this, d] { callAdjacentMemoryCue(d, -1); });
        onPress(cmd(DA::CallNext), [this, d] { callAdjacentMemoryCue(d, 1); });
        onPress(cmd(DA::KeyShiftDown), [this, d] { shiftKey(d, -1); });
        onPress(cmd(DA::KeyShiftUp), [this, d] { shiftKey(d, 1); });
        onPress(cmd(DA::BeatJumpBack), [this, d] { beatJump(d, -1); });
        onPress(cmd(DA::BeatJumpForward), [this, d] { beatJump(d, 1); });
        onPress(cmd(DA::BeatJumpSizeDown), [this, d] { stepJumpSize(d, -1); });
        onPress(cmd(DA::BeatJumpSizeUp), [this, d] { stepJumpSize(d, 1); });
        for (const auto [action, ms] : {std::pair{DA::FineJumpBack, -5.0}, std::pair{DA::FineJumpForward, 5.0}})
            actions.setHandler(cmd(action),
                               [this, d, ms = ms](Phase p)
                               {
                                   if (p != Phase::Release)
                                       engine.send(engine::JumpBy{d, ms / 1000.0}); // held: repeats
                               });
        // Beatgrid (P2-14).
        onPress(cmd(DA::GridDownbeat), [this, d] { editGrid(d, GridEdit::Downbeat); });
        for (const auto [action, edit] : {std::pair{DA::GridEarlier, GridEdit::EarlierMs},
                                          std::pair{DA::GridLater, GridEdit::LaterMs}})
            actions.setHandler(cmd(action),
                               [this, d, edit = edit](Phase p)
                               {
                                   if (p != Phase::Release)
                                       editGrid(d, edit); // held: repeats
                               });
        onPress(cmd(DA::GridBeatBack), [this, d] { editGrid(d, GridEdit::BeatBack); });
        onPress(cmd(DA::GridBeatForward), [this, d] { editGrid(d, GridEdit::BeatForward); });
        onPress(cmd(DA::GridDouble), [this, d] { editGrid(d, GridEdit::Double); });
        onPress(cmd(DA::GridHalve), [this, d] { editGrid(d, GridEdit::Halve); });
        onPress(cmd(DA::GridLock), [this, d] { editGrid(d, GridEdit::ToggleLock); });
        onPress(cmd(DA::TapTempo), [this, d] { tapTempo(d); });
        onPress(cmd(DA::Metronome), [this, d] { toggleMetronome(d); });
        onPress(cmd(DA::HeadphoneCue),
                [this, d]
                {
                    const bool on = !engine.readState().channels[d].cue;
                    engine.send(engine::SetParam{engine::channelParam(d, engine::ChannelParam::Cue), on ? 1.0f : 0.0f});
                });
        onPress(cmd(DA::Load),
                [this, d]
                {
                    if (!libraryBrowser->loadSelected(d)) // nothing selected in the browser: ask for a file
                        chooseTrackForDeck(d);
                });
    }

    actions.setHandler(ui::globalCommand(GA::CrossfaderLeft), [](Phase) {}); // glides every frame while held
    actions.setHandler(ui::globalCommand(GA::CrossfaderRight), [](Phase) {});
    for (const auto [action, position] :
         {std::pair{GA::CrossfaderSnapLeft, 0.0}, std::pair{GA::CrossfaderSnapRight, 1.0},
          std::pair{GA::CrossfaderCentre, 0.5}})
        onPress(ui::globalCommand(action),
                [this, position = position]
                {
                    crossfader = position;
                    sendCrossfader();
                });
    onPress(ui::globalCommand(GA::DebugOverlay), [this] { toggleDebugOverlay(); });
    onPress(ui::globalCommand(GA::PadModeHotCue), [this] { setHotCuePage(1 - hotCuePage); }); // F1: A-H / I-P
    onPress(ui::globalCommand(GA::Quantize),
            [this]
            {
                setQuantize(!quantize, quantizeBeats);
                if (onQuantizeChanged)
                    onQuantizeChanged(quantize, quantizeBeats);
            });
    const auto move = [this](ui::GlobalAction a, int rows)
    {
        actions.setHandler(ui::globalCommand(a),
                           [this, rows](Phase p)
                           {
                               if (p == Phase::Release)
                                   return;
                               const int page = libraryBrowser->pageSize();
                               libraryBrowser->moveSelection(rows > 1 ? page : rows < -1 ? -page : rows);
                           });
    };
    move(GA::BrowserUp, -1);
    move(GA::BrowserDown, 1);
    move(GA::BrowserPageUp, -2);
    move(GA::BrowserPageDown, 2);
    onPress(ui::globalCommand(GA::BrowserCollapse), [this] { libraryBrowser->collapse(); });
    onPress(ui::globalCommand(GA::BrowserExpand), [this] { libraryBrowser->expand(); });
    onPress(ui::globalCommand(GA::BrowserBigger), [this] { toggleBrowserSize(); });
    onPress(ui::globalCommand(GA::BrowserSearch), [this] { libraryBrowser->focusSearch(); });
    onPress(ui::globalCommand(GA::BrowserRemove), [this] { libraryBrowser->removeSelectedFromPlaylist(); });
    for (int stars = 1; stars <= 5; ++stars) // Ctrl+Shift+1..5 (P2-08)
        onPress(ui::globalCommand(static_cast<GA>(static_cast<int>(GA::Rate1) + stars - 1)),
                [this, stars] { libraryBrowser->rateSelected(stars); });
    onPress(ui::globalCommand(GA::RateClear), [this] { libraryBrowser->rateSelected(0); });
    actions.setHandler(ui::globalCommand(GA::KeymapOverlay),
                       [this](Phase p)
                       {
                           if (p == Phase::Press)
                           {
                               keymapOverlay.setBindings(keyboard.currentBindings());
                               keymapOverlay.setVisible(true);
                               keymapOverlay.toFront(false);
                           }
                           else if (p == Phase::Release)
                           {
                               keymapOverlay.setVisible(false);
                           }
                       });
    onPress(ui::globalCommand(GA::FullScreen),
            [this]
            {
                if (auto* window = findParentComponentOfClass<juce::ResizableWindow>())
                    window->setFullScreen(!window->isFullScreen());
            });
    onPress(ui::globalCommand(GA::Panic),
            [this]
            {
                keyboard.releaseAll(); // ends cue previews, bends, glides
                for (engine::DeckId d = 0; d < 2; ++d)
                {
                    engine.send(engine::PitchBend{d, 0.0});
                    engine.send(engine::ScratchEnd{d});
                }
            });
}

void MainComponent::sweepHeldKeys()
{
    using DA = ui::DeckAction;
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    const double dt = std::clamp(now - lastSweepSeconds, 0.0, 0.1);
    lastSweepSeconds = now;

    // Crossfader glide (G / H held): across in 1 s, starting from where the crossfader is.
    const int direction = (keyboard.isHeld(ui::globalCommand(ui::GlobalAction::CrossfaderRight)) ? 1 : 0) -
                          (keyboard.isHeld(ui::globalCommand(ui::GlobalAction::CrossfaderLeft)) ? 1 : 0);
    if (direction != 0)
    {
        if (!crossfaderGliding)
            crossfader = engine.readState().crossfader;
        crossfaderGliding = true;
        crossfader = std::clamp(crossfader + direction * dt, 0.0, 1.0);
        sendCrossfader();
    }
    else
    {
        crossfaderGliding = false;
    }

    // Filter sweep (Shift+3 / Shift+4, Shift+9 / Shift+0 held): half the knob range per second.
    for (engine::DeckId d = 0; d < 2; ++d)
    {
        const int dir = (keyboard.isHeld(ui::deckCommand(d, DA::FilterToHighPass)) ? 1 : 0) -
                        (keyboard.isHeld(ui::deckCommand(d, DA::FilterToLowPass)) ? 1 : 0);
        auto& sweeping = filterSweeping[d];
        if (dir == 0)
        {
            sweeping = false;
            continue;
        }
        if (!sweeping)
            filterKnob[d] = engine.readState().channels[d].filter;
        sweeping = true;
        filterKnob[d] = std::clamp(filterKnob[d] + dir * 0.5 * dt, 0.0, 1.0);
        sendFilter(d);
    }
}

void MainComponent::sendFilter(int channel)
{
    engine.send(engine::SetParam{engine::channelParam(channel, engine::ChannelParam::Filter),
                                 static_cast<float>(filterKnob[static_cast<std::size_t>(channel)])});
}

void MainComponent::sendCrossfader()
{
    engine.send(engine::SetParam{engine::ParamId::Crossfader, static_cast<float>(crossfader)});
}

void MainComponent::setCrossfaderCurve(int curve)
{
    engine.send(engine::SetParam{engine::ParamId::CrossfaderCurve, static_cast<float>(curve)});
}

void MainComponent::setCrossfaderAssign(int deck, int assign)
{
    engine.send(engine::SetParam{engine::channelParam(deck, engine::ChannelParam::CrossfaderAssign),
                                 static_cast<float>(assign)});
}

void MainComponent::setCueMix(double mix)
{
    engine.send(engine::SetParam{engine::ParamId::CueMix, static_cast<float>(mix)});
}

void MainComponent::setHeadphoneLevel(double db)
{
    engine.send(engine::SetParam{engine::ParamId::HeadphoneLevel, static_cast<float>(db)});
}

void MainComponent::setIsolator(bool on)
{
    for (int ch = 0; ch < 2; ++ch)
        engine.send(engine::SetParam{engine::channelParam(ch, engine::ChannelParam::EqMode), on ? 1.0f : 0.0f});
}

void MainComponent::toggleDebugOverlay()
{
    overlay.setVisible(!overlay.isVisible());
    if (overlay.isVisible())
        overlay.toFront(false);
}

void MainComponent::toggleTestTone()
{
    testToneOn = !testToneOn;
    engine.send(engine::SetTestTone{testToneOn, 440.0f, -18.0f});
}

void MainComponent::resetTempo(engine::DeckId deck)
{
    engine.send(engine::ResetTempo{deck});
}

void MainComponent::setTempoRange(double rangePercent)
{
    for (engine::DeckId d = 0; d < 2; ++d)
        engine.send(engine::SetTempoRange{d, rangePercent});
}

void MainComponent::setHighQualityResampling(bool on)
{
    engine.send(engine::SetInterpolationQuality{on ? engine::InterpolationQuality::High
                                                   : engine::InterpolationQuality::Standard});
}

void MainComponent::setStopStyle(int style, double brakeSeconds, double spinBackSeconds)
{
    const auto kind = style == 1 ? engine::StopKind::Brake : style == 2 ? engine::StopKind::SpinBack
                                                                        : engine::StopKind::Instant;
    engine.send(engine::SetStopStyle{kind, static_cast<float>(brakeSeconds), static_cast<float>(spinBackSeconds)});
}

void MainComponent::toggleKeyLock(engine::DeckId deck)
{
    const bool on = engine.readState().decks[deck].keyLock;
    engine.send(engine::SetKeyLock{deck, !on});
}

MainComponent::~MainComponent()
{
    loader.callbacks = {}; // the loader outlives this component; don't let late results call into it
    importJob = nullptr;   // stops a running import (between batches) before the browser goes
    analysisProgress = nullptr;
    analysisService = nullptr; // stops the workers (a track in progress stays not analyzed)
    if (libraryDb != nullptr)
        libraryDb->removeChangeListener(gridListener);
    gridReader = nullptr;
}

juce::File MainComponent::artworkFolder()
{
    return juce::File::getSpecialLocation(juce::File::windowsLocalAppData).getChildFile("Devil-DJ/cache/artwork");
}

juce::File MainComponent::cacheFolder()
{
    return juce::File::getSpecialLocation(juce::File::windowsLocalAppData).getChildFile("Devil-DJ/cache");
}

void MainComponent::loopButton(engine::DeckId deck, ui::DeckView::LoopButton button)
{
    using B = ui::DeckView::LoopButton;
    switch (button)
    {
        case B::In:
            engine.send(engine::Loop{deck, engine::LoopOp::In});
            break;
        case B::Out:
            engine.send(engine::Loop{deck, engine::LoopOp::Out});
            break;
        case B::Auto:
            engine.send(engine::Loop{deck, engine::LoopOp::Auto, 0.0});
            break;
        case B::Halve:
            engine.send(engine::Loop{deck, engine::LoopOp::Halve});
            break;
        case B::Double:
            engine.send(engine::Loop{deck, engine::LoopOp::Double});
            break;
        case B::Reloop:
            engine.send(engine::Loop{deck, engine.readState().decks[deck].loopActive ? engine::LoopOp::Exit
                                                                                       : engine::LoopOp::Reloop});
            break;
        case B::Menu:
            showLoopMenu(deck);
            break;
    }
}

void MainComponent::showLoopMenu(engine::DeckId deck)
{
    const auto& s = engine.readState().decks[deck];
    juce::PopupMenu m;
    m.addSectionHeader("Loop");
    juce::PopupMenu sizes;
    for (const double beats : {1.0 / 64, 1.0 / 32, 1.0 / 16, 1.0 / 8, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0,
                               128.0, 256.0, 512.0})
        sizes.addItem(beats < 1.0 ? "1/" + juce::String(juce::roundToInt(1.0 / beats)) : juce::String(juce::roundToInt(beats)),
                      s.hasGrid, std::abs(beats - s.autoLoopBeats) < 1e-9,
                      [this, deck, beats]
                      {
                          if (engine.readState().decks[deck].loopActive)
                              engine.send(engine::Loop{deck, engine::LoopOp::Exit});
                          engine.send(engine::Loop{deck, engine::LoopOp::Auto, beats});
                      });
    m.addSubMenu("Auto loop of ... beats", sizes, s.hasGrid);
    m.addItem("Save as a memory loop (Ctrl+Z / Ctrl+M)", s.loopActive && deckGrids[deck].trackId != 0, false,
              [this, deck] { addMemoryCue(deck); });
    m.addItem("Export the loop as WAV...", s.loopActive, false, [this, deck] { exportLoop(deck); });
    m.addItem("Exit the loop", s.loopActive, false, [this, deck] { engine.send(engine::Loop{deck, engine::LoopOp::Exit}); });
    m.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

void MainComponent::exportLoop(engine::DeckId deck)
{
    const auto& s = engine.readState().decks[deck];
    const auto track = registry.get(s.bufferId);
    if (!s.loopActive || track == nullptr)
        return;
    const auto start = static_cast<int>(std::llround(s.loopStart)), end = static_cast<int>(std::llround(s.loopEnd));
    const auto name = juce::File::createLegalFileName(track->file.getFileNameWithoutExtension() + " loop " +
                                                      juce::String(s.loopBeats > 0.0 ? juce::String(s.loopBeats, 2) + " beats"
                                                                                     : juce::String("")));
    loopChooser = std::make_unique<juce::FileChooser>(
        "Export the loop as WAV", juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile(name + ".wav"),
        "*.wav");
    loopChooser->launchAsync(
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, track, start, end](const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();
            if (file == juce::File())
                return;
            const auto error = app::writeLoopWav(*track, start, end, file);
            util::logInfo("Loop export {}: {}", file.getFullPathName().toStdString(),
                          error.isEmpty() ? std::string("ok") : error.toStdString());
        });
}

void MainComponent::loadMemoryCues(engine::DeckId deck)
{
    if (deck >= 2)
        return;
    const auto& d = deckGrids[deck];
    const double fileRate = d.fileRate > 0.0 ? d.fileRate : d.bufferRate;
    memoryCues[deck] =
        gridReader != nullptr ? app::loadMemoryCues(*gridReader, d.trackId, fileRate, d.bufferRate) : std::vector<MemoryCue>{};
    showMemoryCues(deck);
}

void MainComponent::showMemoryCues(engine::DeckId deck)
{
    std::vector<std::pair<double, std::uint32_t>> markers;
    std::vector<ui::MemoryCuePanel::Row> rows;
    const double rate = deckGrids[deck].bufferRate > 0.0 ? deckGrids[deck].bufferRate : 48000.0;
    for (const auto& c : memoryCues[deck])
    {
        markers.push_back({c.start, c.colour.value_or(0)});
        const double seconds = c.start / rate;
        const int minutes = static_cast<int>(seconds / 60.0);
        juce::String time = juce::String(minutes) + ":" + juce::String(seconds - 60.0 * minutes, 1).paddedLeft('0', 4);
        juce::String label = juce::String::fromUTF8(c.label.c_str());
        if (c.loop && label.isEmpty())
            label = juce::String(c.length / rate, 2) + " s";
        rows.push_back({time, c.loop, label, c.colour});
    }
    (deck == 0 ? deckA : deckB).setMemoryCues(markers);
    (deck == 0 ? waveA : waveB).setMemoryCues(markers);
    if (auto* panel = memoryPanels[deck].getComponent())
        panel->setRows(std::move(rows), juce::String("Deck ") + engine::deckLetter(deck) + ": memory cues and loops");
}

void MainComponent::editMemoryCue(engine::DeckId deck, std::function<void(library::Connection&)> change)
{
    if (libraryDb == nullptr || deckGrids[deck].trackId == 0)
        return;
    const auto trackId = deckGrids[deck].trackId;
    libraryDb->write(
        [change = std::move(change), safe = juce::Component::SafePointer<MainComponent>(this), deck,
         trackId](library::Connection& c)
        {
            change(c);
            juce::MessageManager::callAsync(
                [safe, deck, trackId]
                {
                    if (safe != nullptr && safe->deckGrids[deck].trackId == trackId) // still the same track
                        safe->loadMemoryCues(deck);
                });
        });
}

void MainComponent::addMemoryCue(engine::DeckId deck)
{
    if (deck >= 2 || !engine.readState().decks[deck].loaded)
        return;
    const auto& d = deckGrids[deck];
    if (d.trackId == 0)
    {
        deckStatus[deck] = "memory cues need the track in the library";
        return;
    }
    const auto& st = engine.readState().decks[deck];
    if (st.loopActive) // a loop plays: store it as a memory loop (P3-03)
    {
        const double fileRate0 = d.fileRate > 0.0 ? d.fileRate : d.bufferRate;
        editMemoryCue(deck,
                      [start = st.loopStart, length = st.loopEnd - st.loopStart, fileRate0, bufferRate = d.bufferRate,
                       trackId = d.trackId](library::Connection& c)
                      {
                          if (const auto r = app::addMemoryCue(c, trackId, true, start, length, fileRate0, bufferRate); !r)
                              util::logInfo("Memory loop: {}", r.error());
                      });
        return;
    }
    // At the playhead; with quantize on, the nearest grid point.
    double at = engine.readState().decks[deck].positionSamples;
    if (quantize && !d.grid.empty())
        at = d.grid.sampleAt(std::round(d.grid.beatAt(at) / quantizeBeats) * quantizeBeats);
    const double fileRate = d.fileRate > 0.0 ? d.fileRate : d.bufferRate;
    const double bufferRate = d.bufferRate;
    editMemoryCue(deck,
                  [at, fileRate, bufferRate, trackId = d.trackId](library::Connection& c)
                  {
                      if (const auto r = app::addMemoryCue(c, trackId, false, at, 0.0, fileRate, bufferRate); !r)
                          util::logInfo("Memory cue: {}", r.error());
                  });
}

void MainComponent::callMemoryCue(engine::DeckId deck, const MemoryCue& cue)
{
    // It becomes the CUE point, and the playhead goes there (quantized while playing, like any jump); a memory
    // loop loops (P3-03).
    engine.send(engine::SetCuePoint{deck, cue.start});
    if (cue.loop && cue.length > 0.0)
        engine.send(engine::Loop{deck, engine::LoopOp::Set, 0.0, cue.start, cue.start + cue.length});
    else
        engine.send(engine::Seek{deck, cue.start});
}

void MainComponent::callAdjacentMemoryCue(engine::DeckId deck, int direction)
{
    if (deck >= 2)
        return;
    const auto& s = engine.readState().decks[deck];
    if (!s.loaded)
        return;
    // "Previous" skips one just behind the playhead (within 0.3 s), so pressing it twice goes further back.
    const double rate = deckGrids[deck].bufferRate > 0.0 ? deckGrids[deck].bufferRate : 48000.0;
    const auto cue = direction > 0 ? nextMemoryCue(memoryCues[deck], s.positionSamples)
                                   : previousMemoryCue(memoryCues[deck], s.positionSamples - 0.3 * rate);
    if (cue)
        callMemoryCue(deck, *cue);
}

void MainComponent::openMemoryPanel(engine::DeckId deck)
{
    if (deck >= 2)
        return;
    auto panel = std::make_unique<ui::MemoryCuePanel>();
    panel->onCall = [this, deck](int row)
    {
        if (row >= 0 && row < static_cast<int>(memoryCues[deck].size()))
            callMemoryCue(deck, memoryCues[deck][static_cast<std::size_t>(row)]);
    };
    panel->onAdd = [this, deck] { addMemoryCue(deck); };
    panel->onDelete = [this, deck](int row)
    {
        if (row < 0 || row >= static_cast<int>(memoryCues[deck].size()))
            return;
        const auto id = memoryCues[deck][static_cast<std::size_t>(row)].id;
        editMemoryCue(deck, [id](library::Connection& c) { removeMemoryCue(c, id); });
    };
    panel->onRename = [this, deck](int row)
    {
        if (row < 0 || row >= static_cast<int>(memoryCues[deck].size()))
            return;
        const auto cue = memoryCues[deck][static_cast<std::size_t>(row)];
        auto* w = new juce::AlertWindow("Memory cue name", {}, juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("name", juce::String::fromUTF8(cue.label.c_str()));
        w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        w->enterModalState(true, juce::ModalCallbackFunction::create(
                                     [this, w, deck, id = cue.id](int result)
                                     {
                                         if (result != 1)
                                             return;
                                         const auto name = w->getTextEditorContents("name").trim().toStdString();
                                         editMemoryCue(deck, [id, name](library::Connection& c) { renameMemoryCue(c, id, name); });
                                     }),
                           true);
    };
    memoryPanels[deck] = panel.get();
    auto& view = deck == 0 ? deckA : deckB;
    juce::CallOutBox::launchAsynchronously(std::move(panel), view.memButton().getScreenBounds(), nullptr);
    showMemoryCues(deck);
}

void MainComponent::loadHotCues(engine::DeckId deck)
{
    if (deck >= 2)
        return;
    const auto& d = deckGrids[deck];
    const double fileRate = d.fileRate > 0.0 ? d.fileRate : d.bufferRate;
    hotCues[deck] = gridReader != nullptr ? app::loadHotCues(*gridReader, d.trackId, fileRate, d.bufferRate) : HotCues{};
    hotCuesInEngine[deck] = false;
    engine.send(toEngine(deck, hotCues[deck]));
    showHotCueLooks(deck);
}

void MainComponent::syncHotCues()
{
    const auto& s = engine.readState();
    for (engine::DeckId deck = 0; deck < 2; ++deck)
    {
        const auto& st = s.decks[deck];
        auto& known = hotCues[deck];
        if (!st.loaded)
            continue;
        if (!hotCuesInEngine[deck])
        {
            // Wait until the engine shows the cues sent for this track (not the last track's).
            bool same = true;
            for (std::size_t i = 0; i < known.size(); ++i)
                same = same && (known[i].set ? std::abs(st.hotCueStart[i] - known[i].start) < 0.5 : st.hotCueStart[i] < 0.0);
            hotCuesInEngine[deck] = same;
            continue;
        }
        const auto& d = deckGrids[deck];
        const double fileRate = d.fileRate > 0.0 ? d.fileRate : d.bufferRate;
        auto changes = diffHotCues(known, st.hotCueStart, st.hotCueLength, fileRate, d.bufferRate);
        if (changes.empty())
            continue;
        showHotCueLooks(deck);
        saveHotCues(deck, std::move(changes));
    }
}

void MainComponent::saveHotCues(engine::DeckId deck, std::vector<HotCueChange> changes)
{
    const auto trackId = deckGrids[deck].trackId;
    if (libraryDb == nullptr || trackId == 0 || changes.empty()) // a file outside the library: this session only
        return;
    libraryDb->write(
        [trackId, changes = std::move(changes)](library::Connection& c)
        {
            if (!applyHotCueChanges(c, trackId, changes))
                util::logWarning("Hot cues: {}", c.lastError());
        });
}

void MainComponent::showHotCueLooks(engine::DeckId deck)
{
    std::array<std::uint32_t, 16> colours{};
    std::array<juce::String, 16> labels;
    for (std::size_t i = 0; i < 16; ++i)
    {
        colours[i] = hotCues[deck][i].set ? hotCues[deck][i].colour : defaultHotCueColour(static_cast<int>(i));
        labels[i] = juce::String::fromUTF8(hotCues[deck][i].label.c_str());
    }
    (deck == 0 ? deckA : deckB).setHotCueLooks(colours, labels);
    (deck == 0 ? waveA : waveB).setHotCueColours(colours);
}

void MainComponent::setHotCuePage(int page)
{
    hotCuePage = std::clamp(page, 0, 1);
    deckA.setHotCuePage(hotCuePage);
    deckB.setHotCuePage(hotCuePage);
}

void MainComponent::showHotCueMenu(engine::DeckId deck, int pad)
{
    if (deck >= 2)
        return;
    const int slot = hotCuePage * 8 + pad;
    const auto& h = hotCues[deck][static_cast<std::size_t>(slot)];
    const auto& d = deckGrids[deck];
    const double fileRate = d.fileRate > 0.0 ? d.fileRate : d.bufferRate;
    juce::PopupMenu m;
    const auto letter = juce::String::charToString(static_cast<juce::juce_wchar>(hotCueLetter(slot)));
    m.addSectionHeader("Hot cue " + letter);
    juce::PopupMenu colours;
    for (const auto& c : ui::kTrackColours)
        colours.addItem(c.name, h.set, h.set && h.colour == c.rgb,
                        [this, deck, slot, rgb = c.rgb, fileRate]
                        {
                            hotCues[deck][static_cast<std::size_t>(slot)].colour = rgb;
                            showHotCueLooks(deck);
                            saveHotCues(deck, {changeFor(hotCues[deck], slot, fileRate, deckGrids[deck].bufferRate)});
                        });
    m.addSubMenu("Colour", colours, h.set);
    m.addItem("Name...", h.set,  false,
              [this, deck, slot, fileRate]
              {
                  auto* w = new juce::AlertWindow("Hot cue name", {}, juce::MessageBoxIconType::NoIcon);
                  w->addTextEditor("name", juce::String::fromUTF8(hotCues[deck][static_cast<std::size_t>(slot)].label.c_str()));
                  w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
                  w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
                  w->enterModalState(true, juce::ModalCallbackFunction::create(
                                               [this, w, deck, slot, fileRate](int result)
                                               {
                                                   if (result == 1)
                                                   {
                                                       auto& cue = hotCues[deck][static_cast<std::size_t>(slot)];
                                                       cue.label = w->getTextEditorContents("name").trim().toStdString();
                                                       showHotCueLooks(deck);
                                                       saveHotCues(deck, {changeFor(hotCues[deck], slot, fileRate,
                                                                                    deckGrids[deck].bufferRate)});
                                                   }
                                               }),
                                     true);
              });
    m.addItem("Delete (Shift + pad)", h.set, false,
              [this, deck, slot] { engine.send(engine::HotCue{deck, static_cast<std::uint8_t>(slot), engine::HotCueOp::Delete}); });
    m.addSeparator();
    m.addItem("Sort hot cues by time", true, false,
              [this, deck, fileRate]
              {
                  hotCues[deck] = sortedByTime(hotCues[deck]);
                  hotCuesInEngine[deck] = false; // until the engine shows the new order
                  engine.send(toEngine(deck, hotCues[deck]));
                  showHotCueLooks(deck);
                  std::vector<HotCueChange> all;
                  for (int i = 0; i < engine::kHotCues; ++i)
                      all.push_back(changeFor(hotCues[deck], i, fileRate, deckGrids[deck].bufferRate));
                  saveHotCues(deck, std::move(all));
              });
    m.addItem(hotCuePage == 0 ? "Show pads I-P (F1)" : "Show pads A-H (F1)", [this] { setHotCuePage(1 - hotCuePage); });
    m.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

void MainComponent::trackHistory()
{
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    const double elapsed = historyLastTick > 0.0 ? std::min(1.0, now - historyLastTick) : 0.0;
    historyLastTick = now;
    if (libraryDb == nullptr)
        return;
    const auto& s = engine.readState();
    syncHotCues(); // P3-01: what the pads changed goes into the library
    for (engine::DeckId d = 0; d < 2; ++d)
    {
        const auto& deck = s.decks[d];
        const auto played = playTracker.update(d, deck.playing && !deck.cuePreview, elapsed);
        if (!played)
            continue;
        const auto at = static_cast<std::int64_t>(juce::Time::currentTimeMillis() / 1000);
        libraryDb->write(
            [session = historySession, trackId = *played, at, deck = static_cast<int>(d)](library::Connection& c)
            {
                library::HistoryRepository history(c);
                if (session->load() == 0)
                    if (const auto id = history.startSession(at))
                        session->store(id.value());
                if (session->load() != 0)
                    if (!history.recordPlay(session->load(), trackId, at, deck))
                        util::logWarning("History: {}", history.error());
            });
    }
}

void MainComponent::showAnalysisProgress()
{
    if (analysisService == nullptr)
        return;
    const auto s = analysisService->status();
    if (s.idle())
    {
        if (analysisTotal > 0)
            util::logInfo("Analysis: done ({} analyzed, {} failed)", s.done, s.failed);
        analysisTotal = 0;
        libraryBrowser->setActivity({});
        return;
    }
    // "Analyzing 12 of 500" - the total grows when an import adds tracks.
    analysisTotal = std::max(analysisTotal, s.done + s.failed + s.running + s.queued);
    const int finished = std::min(analysisTotal, s.done + s.failed);
    libraryBrowser->setActivity("Analyzing " + juce::String(finished + 1) + " of " + juce::String(analysisTotal));
}

void MainComponent::toggleBrowserSize()
{
    browserBig = !browserBig;
    libraryBrowser->setBig(browserBig);
    resized();
}

bool MainComponent::isImporting() const
{
    return importJob != nullptr && importJob->isThreadRunning();
}

void MainComponent::checkMissingFiles(bool quiet)
{
    if (libraryDb == nullptr)
        return;
    if (isImporting())
    {
        if (!quiet)
            libraryBrowser->setStatus("The library is busy - try again in a moment");
        return;
    }
    auto* db = libraryDb;
    importJob = std::make_unique<LibraryTask>(
        "Missing files",
        [db, quiet](LibraryTask& task)
        {
            if (!quiet)
                task.post("Checking every track's file...");
            const auto r = library::scanForMissingFiles(*db, [&task] { return task.cancelled(); });
            util::logInfo("Missing files: {} checked, {} missing ({} new, {} found again)", r.checked, r.missing,
                          r.newlyMissing, r.foundAgain);
            if (r.missing == 0)
                return quiet ? juce::String() : juce::String("All " + juce::String(r.checked) + " files are there");
            return juce::String(r.missing) + (r.missing == 1 ? " track's file is" : " tracks' files are") +
                   " missing - see Missing files (right-click there to relocate)";
        },
        *libraryBrowser);
    importJob->startThread(juce::Thread::Priority::low);
}

void MainComponent::relocateMissingFiles(const std::vector<std::int64_t>& trackIds)
{
    if (libraryDb == nullptr)
        return;
    if (isImporting())
    {
        libraryBrowser->setStatus("The library is busy - try again in a moment");
        return;
    }
    importChooser = std::make_unique<juce::FileChooser>("Where are the missing files now? (searched with subfolders)",
                                                        juce::File::getSpecialLocation(juce::File::userMusicDirectory));
    importChooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [this, trackIds](const juce::FileChooser& fc)
        {
            const auto folder = fc.getResult();
            if (!folder.isDirectory() || libraryDb == nullptr || isImporting())
                return;
            auto* db = libraryDb;
            importJob = std::make_unique<LibraryTask>(
                "Relocate",
                [db, trackIds, where = toPath(folder)](LibraryTask& task)
                {
                    task.post("Searching " + juce::String(where.wstring().c_str()) + "...");
                    const auto r = library::relocateMissingFiles(*db, where, trackIds, [&task] { return task.cancelled(); });
                    util::logInfo("Relocate: {} looked for, {} found, {} not found, {} files searched", r.lookedFor,
                                  r.relocated, r.notFound, r.filesIndexed);
                    for (const auto& n : r.notes)
                        util::logWarning("Relocate: {}", n);
                    if (r.lookedFor == 0)
                        return juce::String("No missing files to look for");
                    juce::String text;
                    text << "Found " << r.relocated << " of " << r.lookedFor << " missing files";
                    if (r.notFound > 0)
                        text << " (" << r.notFound << " not in that folder)";
                    return text;
                },
                *libraryBrowser);
            importJob->startThread(juce::Thread::Priority::low);
        });
}

void MainComponent::importOtherLibrary()
{
    if (libraryDb == nullptr)
        return;
    importChooser = std::make_unique<juce::FileChooser>(
        "Import a library from other software (DJ library XML or iTunes / Apple Music Library.xml)",
        juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.xml");
    importChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                               [this](const juce::FileChooser& fc)
                               {
                                   if (const auto file = fc.getResult(); file.existsAsFile())
                                       importToLibrary({file});
                               });
}

void MainComponent::importPlaylistFile(std::optional<std::int64_t> parentFolder)
{
    if (libraryDb == nullptr)
        return;
    importChooser = std::make_unique<juce::FileChooser>(
        "Import a playlist", juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.m3u8;*.m3u");
    importChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                               [this, parentFolder](const juce::FileChooser& fc)
                               {
                                   if (const auto file = fc.getResult(); file.existsAsFile())
                                       importToLibrary({file}, parentFolder);
                               });
}

void MainComponent::importToLibrary(const juce::Array<juce::File>& filesOrFolders,
                                    std::optional<std::int64_t> playlistParent)
{
    if (libraryDb == nullptr)
        return;
    if (isImporting())
    {
        libraryBrowser->setStatus("Still adding the last folder - try again when it is done");
        return;
    }
    if (filesOrFolders.isEmpty())
    {
        importChooser = std::make_unique<juce::FileChooser>(
            "Add a music folder to the library", juce::File::getSpecialLocation(juce::File::userMusicDirectory));
        importChooser->launchAsync(juce::FileBrowserComponent::openMode |
                                       juce::FileBrowserComponent::canSelectDirectories,
                                   [this](const juce::FileChooser& fc)
                                   {
                                       if (const auto folder = fc.getResult(); folder.isDirectory())
                                           importToLibrary({folder});
                                   });
        return;
    }
    importJob = std::make_unique<ImportJob>(*libraryDb, filesOrFolders, artworkFolder(), *libraryBrowser, playlistParent,
                                            analysisService.get());
    importJob->startThread(juce::Thread::Priority::low);
}

void MainComponent::chooseTrackForDeck(engine::DeckId deck)
{
    juce::StringArray patterns;
    for (const auto& ext : engine::supportedTrackExtensions())
        patterns.add("*" + ext);

    chooser = std::make_unique<juce::FileChooser>(juce::String("Load track to deck ") + engine::deckLetter(deck),
                                                  juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                  patterns.joinIntoString(";"));
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this, deck](const juce::FileChooser& fc)
                         {
                             const auto file = fc.getResult();
                             if (file == juce::File())
                                 return;
                             loadFile(deck, file);
                         });
}

void MainComponent::loadFile(engine::DeckId deck, const juce::File& file)
{
    if (!file.existsAsFile())
        return;
    deckStatus[deck] = "loading " + file.getFileName();
    loader.load(deck, file, audio.getStatus().sampleRate);
}

void MainComponent::unloadDeck(engine::DeckId deck)
{
    loader.unload(deck);
    deckStatus[deck] = "empty";
}

namespace
{
juce::String clock(double samples, double rate)
{
    if (rate <= 0.0)
        return "--:--.-";
    const double seconds = samples / rate;
    const int minutes = static_cast<int>(seconds / 60.0);
    const double rest = seconds - minutes * 60.0;
    return juce::String(minutes).paddedLeft('0', 2) + ":" + juce::String(rest, 1).paddedLeft('0', 4);
}
} // namespace

namespace
{
juce::String levelDb(float linear)
{
    return linear < 1e-5f ? juce::String("-inf") : juce::String(20.0 * std::log10(linear), 1);
}
} // namespace

juce::StringArray MainComponent::statusLines()
{
    juce::StringArray lines;
    const auto& state = engine.readState();
    // Master clock (P2-18): who leads, tempo, bar.beat.
    {
        const double beat = state.masterBeat;
        const auto bar = static_cast<long long>(std::floor(beat / 4.0)) + 1;
        const double inBar = beat - 4.0 * static_cast<double>(bar - 1) + 1.0;
        lines.add("Master clock: " + juce::String(state.masterBpm, 2) + " BPM, " +
                  (state.masterSource >= 0 ? juce::String("deck ") + engine::deckLetter(
                                                 static_cast<engine::DeckId>(state.masterSource))
                                           : juce::String("internal")) +
                  ", bar " + juce::String(bar) + " beat " + juce::String(inBar, 2));
    }
    for (engine::DeckId d = 0; d < deckStatus.size(); ++d)
    {
        lines.add(juce::String("Deck ") + engine::deckLetter(d) + ": " + deckStatus[d]);
        const auto& s = state.decks[d];
        if (s.loaded)
        {
            const juce::String what = s.cuePreview ? "CUE" : s.playing ? "PLAY" : s.atEnd ? "END" : "pause";
            lines.add("   " + what + "  " + clock(s.positionSamples, state.sampleRate) + " / " +
                      clock(s.lengthSamples, state.sampleRate) + "  cue " + clock(s.cuePointSamples, state.sampleRate));
            if (s.synced)
                lines.add("   SYNC" + juce::String(s.master ? " (master)" : "") + "  phase " +
                          juce::String(s.phaseErrorMs, 2) + " ms");
            lines.add(juce::String("   ") + (s.scratching ? "SCRATCH  " : "") + (s.keyLock ? "KEY LOCK  " : "") + (s.slip ? (s.slipping ? "SLIPPING  " : "SLIP  ") : "") + (s.reverse ? "REVERSE  " : "") +
                      (s.stopping == 1 ? "BRAKE  " : s.stopping == 2 ? "SPIN-BACK  " : "") +
                      "tempo " + juce::String(s.tempoPercent >= 0 ? "+" : "") + juce::String(s.tempoPercent, 1) +
                      " % (range +-" + juce::String(juce::roundToInt(s.tempoRangePercent)) + ")  bend " +
                      juce::String(s.pitchBendPercent, 1) + " %  rate " + juce::String(s.rate, 4));
        }
        const auto& c = state.channels[d];
        const auto band = [](float db) { return std::isinf(db) ? juce::String("KILL") : juce::String(db, 1); };
        const double knob = c.filter;
        const juce::String filter =
            std::abs(knob - 0.5) <= engine::ChannelStrip::kFilterDeadZone ? juce::String("off")
            : knob < 0.5
                ? "LP " + juce::String(juce::roundToInt(engine::ChannelStrip::filterTarget(knob).lowPassHz)) + " Hz"
                : "HP " + juce::String(juce::roundToInt(engine::ChannelStrip::filterTarget(knob).highPassHz)) + " Hz";
        lines.add(juce::String("   ") + (c.eqMode == 1 ? "ISO" : "EQ") + "  low " + band(c.eqDb[0]) + "  mid " +
                  band(c.eqDb[1]) + "  high " + band(c.eqDb[2]) + "  filter " + filter);
        static const char* const sides[] = {"xf A", "xf THRU", "xf B"};
        lines.add(juce::String("   ") + (c.cue ? "CUE  " : "") + sides[std::min<int>(c.crossfaderAssign, 2)] +
                  "  meter " + levelDb(std::max(c.peakL, c.peakR)) + " dB peak, " + levelDb(c.rms) + " dB rms");
    }
    lines.add("Keys A: Z cue, X play, 1/2 bend, Shift+1/2 tempo, Shift+Z start, Ctrl+W key lock");
    lines.add("Keys B: M cue, , play, 7/8 bend, Shift+7/8 tempo, Shift+M start, Ctrl+I key lock");
    static const char* const curves[] = {"smooth", "linear", "cut"};
    lines.add("Master: crossfader " + juce::String(state.crossfader, 2) + " (" +
              curves[std::min<int>(state.crossfaderCurve, 2)] + ")  peak " + levelDb(state.masterPeakL) + " / " +
              levelDb(state.masterPeakR) + " dB  limiter -" + juce::String(state.limiterGainReductionDb, 1) + " dB");
    lines.add("Headphones: mix " + juce::String(state.cueMix, 2) + " (0 cue .. 1 master)  level " +
              juce::String(state.headphoneLevelDb, 0) + " dB  peak " +
              levelDb(std::max(state.cuePeakL, state.cuePeakR)) + " dB" +
              (state.cueRouted ? "" : "  [not routed: Options > Audio settings > output layout]"));
    lines.add("Crossfader: hold G / H glide, Shift+G / Shift+H snap, B centre;  CUE: Ctrl+E (A), Ctrl+O (B)");
    if (soak.isRunning())
        lines.add(soak.statusLine());
    lines.add("Mixer: Ctrl+A/S/D, Ctrl+J/K/L kill; hold Shift+3/4, Shift+9/0 filter; Shift+5, Shift+- reset");
    lines.add("Waveforms: drag = scratch (playing) / scrub (paused), wheel = nudge, Ctrl+wheel = zoom");
    for (const auto* w : {&waveA, &waveB})
    {
        const auto f = w->frameStats();
        lines.add(juce::String("   frame: paint ") + juce::String(f.paintMsAverage, 2) + " ms avg, " +
                  juce::String(f.paintMsMax, 2) + " ms max; " +
                  (f.frameIntervalMs > 0.0 ? juce::String(1000.0 / f.frameIntervalMs, 1) : juce::String("--")) +
                  " fps");
    }
    lines.add("Track buffers: " + juce::String(engine::TrackBuffer::liveCount()) + " alive, " +
              juce::String(static_cast<int>(registry.size())) + " registered, " +
              juce::String(juce::roundToInt(static_cast<double>(registry.memoryBytes()) / (1024.0 * 1024.0))) + " MB");
    return lines;
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    // Temporary build-info line; moves to the About box later.
    const auto version = TagLib::runtimeVersion();
    const auto info = juce::String("JUCE ") + juce::String(JUCE_MAJOR_VERSION) + "." +
                      juce::String(JUCE_MINOR_VERSION) + "." + juce::String(JUCE_BUILDNUMBER) + "  |  TagLib " +
                      juce::String(version.majorVersion()) + "." + juce::String(version.minorVersion()) + "." +
                      juce::String(version.patchVersion());
    g.setColour(juce::Colours::white.withAlpha(0.35f));
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(info, getLocalBounds().reduced(12), juce::Justification::bottomLeft);
}

juce::File MainComponent::soakFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Devil-DJ/Soak");
}

void MainComponent::startSoak(double minutes, bool silent)
{
    const auto error = soak.start({minutes, silent});
    if (error.isNotEmpty())
    {
        util::logWarning("Soak test not started: {}", error.toStdString());
        if (!quitAfterSoak)
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Soak test", error);
        else
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        return;
    }
    util::logInfo("Soak test started: {} min{}", minutes, silent ? " (silent)" : "");
    if (!overlay.isVisible())
        toggleDebugOverlay(); // show progress
}

void MainComponent::stopSoak()
{
    soak.stop();
}

void MainComponent::startSoakWhenLoaded(double minutes, bool silent, bool quitWhenDone)
{
    quitAfterSoak = quitWhenDone;
    const double deadline = juce::Time::getMillisecondCounterHiRes() + 120000.0;
    soakWaiter = std::make_unique<juce::TimedCallback>(
        [this, minutes, silent, deadline]
        {
            const auto& s = engine.readState();
            const bool loaded = s.decks[0].loaded && s.decks[1].loaded;
            if (!loaded && juce::Time::getMillisecondCounterHiRes() < deadline)
                return;
            soakWaiter->stopTimer();
            startSoak(minutes, silent);
        });
    soakWaiter->startTimer(500);
}

void MainComponent::soakFinished(const juce::String& report)
{
    const auto folder = soakFolder();
    folder.createDirectory();
    const auto file = folder.getChildFile("soak-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S") + ".txt");
    file.replaceWithText(report);
    util::logInfo("Soak test finished ({}): {}", soak.passed() ? "pass" : "FAIL", file.getFullPathName().toStdString());
    if (quitAfterSoak)
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
        return;
    }
    juce::AlertWindow::showMessageBoxAsync(
        soak.passed() ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
        soak.passed() ? "Soak test passed" : "Soak test: glitches",
        report.upToFirstOccurrenceOf("\nminute", false, false) + "\nFull report: " + file.getFullPathName());
}

juce::File MainComponent::keymapFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Devil-DJ/Keymaps");
}

void MainComponent::reloadKeymap()
{
    const auto file = keymapFolder().getChildFile("keymap.json");
    auto bindings = ui::defaultKeymap();
    juce::String status = "default keys";
    if (file.existsAsFile())
    {
        std::vector<std::string> errors;
        bindings = ui::applyKeymapOverrides(std::move(bindings), file.loadFileAsString().toStdString(), errors);
        status =
            "keymap.json loaded" +
            (errors.empty() ? juce::String() : ", " + juce::String(static_cast<int>(errors.size())) + " problem(s)");
        for (const auto& e : errors)
            util::logWarning("Keymap {}: {}", file.getFullPathName().toStdString(), e);
        util::logInfo("Keymap: {} ({} problems)", file.getFullPathName().toStdString(), errors.size());
    }
    keyboard.setBindings(std::move(bindings));
    keymapOverlay.setStatus(status);
    keymapOverlay.setBindings(keyboard.currentBindings());
}

void MainComponent::openKeymapFolder()
{
    const auto folder = keymapFolder();
    folder.createDirectory();
    const auto example = folder.getChildFile("keymap.example.json");
    if (!example.existsAsFile())
        example.replaceWithText(juce::String::fromUTF8(ui::exampleKeymapJson().c_str()));
    folder.startAsProcess();
}

double MainComponent::outputLatencySeconds() const
{
    return audio.getStatus().totalOutputLatencyMs / 1000.0;
}

void MainComponent::resized()
{
    // Top: deck A | mixer | deck B; then the two zoomed waveforms; bottom: the file browser. On short windows
    // (laptops at 125-150 % scaling) the mixer gives up height first, down to its minimum, so the waveforms keep
    // at least ~80 px each and the browser ~4 rows.
    auto area = getLocalBounds().reduced(12).withTrimmedBottom(18); // bottom: the build-info line
    const int topHeight = std::clamp(area.getHeight() - 2 * 80 - 6 - 130 - 14, ui::MixerView::kMinimumHeight,
                                     ui::MixerView::kPreferredHeight);
    auto top = area.removeFromTop(topHeight);
    area.removeFromTop(8);
    const int deckWidth = (top.getWidth() - ui::MixerView::kPreferredWidth - 12) / 2;
    const int deckHeight = std::min(300, top.getHeight()); // with the pad and loop rows (P3-01, P3-03)
    deckA.setBounds(top.removeFromLeft(deckWidth).withHeight(deckHeight));
    top.removeFromLeft(6);
    deckB.setBounds(top.removeFromRight(deckWidth).withHeight(deckHeight));
    top.removeFromRight(6);
    mixer.setBounds(top);

    waveA.setVisible(!browserBig);
    waveB.setVisible(!browserBig);
    if (browserBig) // Ctrl+B: the browser gets the waveforms' room too
    {
        libraryBrowser->setBounds(area);
        keymapOverlay.setBounds(getLocalBounds());
        overlay.setBounds(getWidth() - ui::DebugOverlay::kWidth - kOverlayMargin, kOverlayMargin,
                          ui::DebugOverlay::kWidth, ui::DebugOverlay::kHeight);
        return;
    }
    const int browserHeight = std::max(160, area.getHeight() - 2 * 100 - 12); // waveforms up to ~100 px each
    libraryBrowser->setBounds(area.removeFromBottom(browserHeight));
    area.removeFromBottom(6);
    const int waveHeight = (area.getHeight() - 6) / 2;
    waveA.setBounds(area.removeFromTop(waveHeight));
    area.removeFromTop(6);
    waveB.setBounds(area.removeFromTop(waveHeight));

    keymapOverlay.setBounds(getLocalBounds());
    overlay.setBounds(getWidth() - ui::DebugOverlay::kWidth - kOverlayMargin, kOverlayMargin, ui::DebugOverlay::kWidth,
                      ui::DebugOverlay::kHeight);
}

// ---- beatgrids (P2-14) ----

util::Beatgrid MainComponent::deckGrid(engine::DeckId deck) const
{
    return deck < deckGrids.size() ? deckGrids[deck].grid : util::Beatgrid();
}

std::int64_t MainComponent::deckTrackId(engine::DeckId deck) const
{
    return deck < deckGrids.size() ? deckGrids[deck].trackId : 0;
}

void MainComponent::sendGrid(engine::DeckId deck)
{
    engine.send(engine::SetBeatgrid{deck, deckGrids[deck].grid});
}

void MainComponent::loadGridFor(engine::DeckId deck, const juce::File& file, double bufferRate)
{
    if (deck >= deckGrids.size())
        return;
    auto& d = deckGrids[deck];
    d = {};
    d.bufferRate = bufferRate;
    showDeckKey(deck); // none until the library says
    d.autoGainPending = autoGain;
    d.autoGainAtLoad = autoGain;
    if (gridReader != nullptr)
        if (auto* s = gridReader->prepare("SELECT id, sample_rate FROM tracks WHERE path = ? COLLATE NOCASE"))
        {
            if (s->bind(1, library::libraryPath(toPath(file))).step())
            {
                d.trackId = s->getInt(0);
                d.fileRate = static_cast<double>(s->getOptionalInt(1).value_or(0));
            }
            s->reset();
        }
    playTracker.loaded(deck, d.trackId); // P2-23: the history counts library tracks
    loadHotCues(deck);                   // P3-01
    loadMemoryCues(deck);                // P3-02
    reloadGrid(deck);
    if (d.metronome)
        engine.send(engine::SetMetronome{deck, true});
}

void MainComponent::showDeckKey(engine::DeckId deck)
{
    if (deck >= deckGrids.size())
        return;
    (deck == 0 ? deckA : deckB).setTrackKey(deckGrids[deck].musicalKey, keyNotation);
}

void MainComponent::setKeyNotation(util::KeyNotation notation)
{
    keyNotation = notation;
    if (libraryBrowser != nullptr)
        libraryBrowser->setKeyNotation(notation);
    for (engine::DeckId d = 0; d < 2; ++d)
        showDeckKey(d);
}

void MainComponent::keySync(engine::DeckId deck)
{
    if (deck >= 2)
        return;
    const auto& s = engine.readState();
    // The master: the deck leading the clock, else the other deck.
    int master = s.masterSource;
    if (master < 0 || master == deck || master >= 2)
        master = 1 - deck;
    const auto& m = s.decks[static_cast<std::size_t>(master)];
    const int masterKey = util::heardKey(deckGrids[static_cast<std::size_t>(master)].musicalKey, m.keyLock, m.keyShift,
                                         1.0 + m.tempoPercent / 100.0);
    const auto shift = util::keySyncShift(deckGrids[deck].musicalKey, masterKey, keySyncCompatible);
    if (!shift || !s.decks[deck].loaded || !m.loaded)
        return;
    engine.send(engine::SetKeyLock{deck, true});
    engine.send(engine::SetKeyShift{deck, *shift});
}

void MainComponent::shiftKey(engine::DeckId deck, int semitones)
{
    if (deck >= 2)
        return;
    const auto& s = engine.readState().decks[deck];
    engine.send(engine::SetKeyLock{deck, true});
    engine.send(engine::SetKeyShift{deck, std::clamp(s.keyShift + semitones, -12, 12)});
}

juce::String MainComponent::jumpSizeText(double beats)
{
    return beats < 1.0 ? "1/" + juce::String(juce::roundToInt(1.0 / beats)) : juce::String(juce::roundToInt(beats));
}

double MainComponent::jumpSize(engine::DeckId deck) const noexcept
{
    return kJumpSizes[static_cast<std::size_t>(jumpSizeIndex[deck])];
}

void MainComponent::beatJump(engine::DeckId deck, int direction)
{
    if (deck < 2)
        engine.send(engine::BeatJump{deck, direction * jumpSize(deck)});
}

void MainComponent::stepJumpSize(engine::DeckId deck, int steps)
{
    if (deck >= 2)
        return;
    jumpSizeIndex[deck] = std::clamp(jumpSizeIndex[deck] + steps, 0, static_cast<int>(std::size(kJumpSizes)) - 1);
    (deck == 0 ? deckA : deckB).setJumpSizeText(jumpSizeText(jumpSize(deck)));
}

void MainComponent::showJumpSizeMenu(engine::DeckId deck)
{
    if (deck >= 2)
        return;
    juce::PopupMenu m;
    m.addSectionHeader("Beat jump size (beats)");
    for (int i = 0; i < static_cast<int>(std::size(kJumpSizes)); ++i)
        m.addItem(jumpSizeText(kJumpSizes[static_cast<std::size_t>(i)]), true, i == jumpSizeIndex[deck],
                  [this, deck, i] { stepJumpSize(deck, i - jumpSizeIndex[deck]); });
    m.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

void MainComponent::setQuantize(bool on, double beats)
{
    quantize = on;
    quantizeBeats = beats;
    engine.send(engine::SetQuantize{on, static_cast<float>(beats)});
}

void MainComponent::setAutoGain(bool on, double targetLufs)
{
    autoGain = on;
    autoGainTarget = targetLufs;
}

void MainComponent::applyAutoGain(engine::DeckId deck, bool atLoad)
{
    auto& d = deckGrids[deck];
    const auto& state = engine.readState();
    const float trimNow = state.channels[deck].trimDb;
    // After the load: only while the deck is stopped and nobody moved the trim since Auto Gain set it.
    if (!atLoad && (state.decks[deck].playing || std::abs(trimNow - d.autoGainTrim) > 0.01f))
    {
        d.autoGainPending = false;
        return;
    }
    const float trim =
        d.loudnessLufs ? static_cast<float>(analysis::autoGainDb(*d.loudnessLufs, autoGainTarget)) : 0.0f;
    engine.send(engine::SetParam{engine::channelParam(deck, engine::ChannelParam::Trim), trim});
    d.autoGainTrim = trim;
    d.autoGainPending = !d.loudnessLufs.has_value(); // not analyzed yet: again when the result comes
}

void MainComponent::reloadGrid(engine::DeckId deck)
{
    if (deck >= deckGrids.size() || gridReader == nullptr)
        return;
    auto& d = deckGrids[deck];
    if (d.trackId == 0 || d.bufferRate <= 0.0)
        return;
    // The key too (analysis results arrive here, P2-15).
    if (auto* k = gridReader->prepare("SELECT musical_key, loudness_lufs FROM tracks WHERE id = ?"))
    {
        const bool found = k->bind(1, d.trackId).step();
        d.musicalKey = found ? static_cast<int>(k->getOptionalInt(0).value_or(-1)) : -1;
        d.loudnessLufs = found ? k->getOptionalDouble(1) : std::nullopt;
        k->reset();
        showDeckKey(deck);
        if (d.autoGainPending)
        {
            applyAutoGain(deck, d.autoGainAtLoad);
            d.autoGainAtLoad = false;
        }
    }
    util::Beatgrid grid;
    if (const auto stored = library::BeatgridRepository(*gridReader).get(d.trackId))
    {
        if (stored->sampleRate)
            d.fileRate = *stored->sampleRate;
        const double fileRate = d.fileRate > 0.0 ? d.fileRate : d.bufferRate;
        if (auto parsed = util::Beatgrid::fromJson(stored->anchorsJson, fileRate))
        {
            grid = parsed->rescaled(d.bufferRate);
            grid.locked = stored->locked;
        }
    }
    if (grid == d.grid)
        return;
    d.grid = grid;
    sendGrid(deck);
}

void MainComponent::saveGrid(engine::DeckId deck)
{
    auto& d = deckGrids[deck];
    if (libraryDb == nullptr || d.trackId == 0)
        return;
    const auto stored = d.grid.rescaled(d.fileRate > 0.0 ? d.fileRate : d.bufferRate);
    const auto json = stored.toJson();
    const bool locked = d.grid.locked;
    const double bpm = stored.firstBpm();
    const auto id = d.trackId;
    libraryDb->write([id, json, locked, bpm](library::Connection& c)
                     { library::BeatgridRepository(c).set(id, json, locked, bpm > 0.0 ? std::optional<double>(bpm) : std::nullopt); });
}

void MainComponent::editGrid(engine::DeckId deck, GridEdit edit)
{
    if (deck >= deckGrids.size())
        return;
    auto& d = deckGrids[deck];
    auto& g = d.grid;
    if (g.empty())
    {
        libraryBrowser->setStatus(juce::String("Deck ") + engine::deckLetter(deck) +
                                  " has no beatgrid yet (it appears when the track is analyzed, or tap the tempo)");
        return;
    }
    const double position = engine.readState().decks[deck].positionSamples;
    const double oneMs = 0.001 * d.bufferRate;
    switch (edit)
    {
        case GridEdit::Downbeat: g.setDownbeatAt(position); break;
        case GridEdit::EarlierMs: g.shiftSamples(-oneMs); break;
        case GridEdit::LaterMs: g.shiftSamples(oneMs); break;
        case GridEdit::BeatBack: g.shiftBeats(-1); break;
        case GridEdit::BeatForward: g.shiftBeats(1); break;
        case GridEdit::Double: g.scaleTempo(2.0); break;
        case GridEdit::Halve: g.scaleTempo(0.5); break;
        case GridEdit::ToggleLock: g.locked = !g.locked; break;
    }
    if (edit != GridEdit::ToggleLock)
        g.locked = true; // a grid fixed by hand: analysis leaves it alone
    sendGrid(deck);
    saveGrid(deck);
}

void MainComponent::tapTempo(engine::DeckId deck, double nowSeconds)
{
    if (deck >= deckGrids.size())
        return;
    auto& d = deckGrids[deck];
    const auto& s = engine.readState().decks[deck];
    if (!s.loaded || d.bufferRate <= 0.0)
        return;
    d.taps.tap(nowSeconds >= 0.0 ? nowSeconds : juce::Time::getMillisecondCounterHiRes() / 1000.0);
    const auto played = d.taps.bpm();
    if (!played)
    {
        libraryBrowser->setStatus("Tap " + juce::String(d.taps.count()) + " - keep tapping in time");
        return;
    }
    // The taps are in the track as it plays: take the tempo fader out; a beat sits at the playhead now.
    const double trackBpm = std::round(*played / (1.0 + s.tempoPercent / 100.0) * 100.0) / 100.0;
    const bool wasLocked = d.grid.locked;
    d.grid = util::Beatgrid::constant(s.positionSamples, trackBpm, d.bufferRate);
    d.grid.locked = true;
    juce::ignoreUnused(wasLocked);
    sendGrid(deck);
    saveGrid(deck);
    libraryBrowser->setStatus(juce::String("Deck ") + engine::deckLetter(deck) + ": " + juce::String(trackBpm, 2) +
                              " BPM from " + juce::String(d.taps.count()) + " taps");
}

void MainComponent::toggleMetronome(engine::DeckId deck)
{
    if (deck >= deckGrids.size())
        return;
    auto& d = deckGrids[deck];
    d.metronome = !d.metronome;
    engine.send(engine::SetMetronome{deck, d.metronome});
}

void MainComponent::showGridMenu(engine::DeckId deck)
{
    juce::PopupMenu m;
    const bool hasGrid = !deckGrid(deck).empty();
    const char* keys[][2] = {{"Z", "M"}, {"A", "J"}, {"S", "K"}, {"Q", "U"}, {"W", "I"},
                             {"E", "O"}, {"R", "P"}, {"C", "."},  {"V", "/"}};
    const auto key = [&](int row) { return juce::String("   (Ctrl+Shift+") + keys[row][deck == 0 ? 0 : 1] + ")"; };
    const auto add = [&](const juce::String& text, int row, GridEdit edit)
    { m.addItem(text + key(row), hasGrid, false, [this, deck, edit] { editGrid(deck, edit); }); };
    add("Set downbeat here", 0, GridEdit::Downbeat);
    add("Grid 1 ms earlier", 1, GridEdit::EarlierMs);
    add("Grid 1 ms later", 2, GridEdit::LaterMs);
    add("Downbeat one beat earlier", 3, GridEdit::BeatBack);
    add("Downbeat one beat later", 4, GridEdit::BeatForward);
    add("BPM x2", 5, GridEdit::Double);
    add("BPM /2", 6, GridEdit::Halve);
    m.addSeparator();
    m.addItem("Metronome in the headphones" + key(7), hasGrid, deckGrids[deck].metronome,
              [this, deck] { toggleMetronome(deck); });
    m.addItem("Lock the grid (analysis leaves it alone)" + key(8), hasGrid, deckGrid(deck).locked,
              [this, deck] { editGrid(deck, GridEdit::ToggleLock); });
    m.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

} // namespace devildj::app
