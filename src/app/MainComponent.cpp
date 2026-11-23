#include "MainComponent.h"

#include "analysis/Loudness.h"
#include "matcher/CandidateQuery.h"
#include "beatmaker/DrumKits.h"
#include "beatmaker/ProjectFile.h"
#include "beatmaker/SynthPresets.h"
#include "app/BeatRender.h"
#include "app/AutoTransition.h"
#include "app/TransitionPresets.h"
#include "app/TransitionStore.h"
#include "library/HistoryRepository.h"
#include "ui/TrackColours.h"

#include "engine/TrackDecoder.h"
#include "library/Database.h"
#include "library/ForeignLibrary.h"
#include "library/Importer.h"
#include "library/BeatgridRepository.h"
#include "library/MissingFiles.h"
#include "library/PlaylistFiles.h"
#include "library/TrackRepository.h"
#include "MainLayout.h"
#include "RecordingSheets.h"
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
    // FX (P3-09): FX1 on deck A, FX2 on deck B, three effects each to start.
    addAndMakeVisible(fxPanel);
    fxPanel.onEffect = [this](int unit, int slot, int type) { chooseEffect(unit, slot, type); };
    // Sampler (P3-13): its panel shares the FX panel's place.
    addChildComponent(samplerPanel);
    addChildComponent(recorderPanel); // P3-16
    recorderPanel.onRecord = [this](bool) { toggleRecording(); };
    recorderPanel.onOpenFolder = [] { recordingsFolder().createDirectory(); recordingsFolder().startAsProcess(); };
    recorderPanel.onSettingsChanged = [this](const ui::RecorderPanel::Settings& s)
    {
        if (onRecorderSettingsChanged)
            onRecorderSettingsChanged(s);
    };
    recorder.onFileClosed = [safe = juce::Component::SafePointer<MainComponent>(this)](const engine::Recorder::FileInfo& f)
    {
        juce::MessageManager::callAsync([safe, f] { if (safe != nullptr) safe->writeRecordingSheets(f); });
    };
    // Automix (P3-18).
    addChildComponent(automixPanel);
    automixPanel.onToggle = [this] { toggleAutomix(); };
    automixPanel.onQueue = [this] { queueAutomix(); };
    automixPanel.onSettingsChanged = [this](const ui::AutomixPanel::Settings& s)
    {
        applyAutomixSettings();
        if (onAutomixSettingsChanged)
            onAutomixSettingsChanged(s);
    };
    automix.load = [this](engine::DeckId deck, int index)
    {
        if (deck > 1 || index < 0 || index >= static_cast<int>(automixFiles.size()))
            return;
        automixLoading[deck] = index;
        loadFile(deck, automixFiles[static_cast<std::size_t>(index)]);
    };
    automix.gridOf = [this](engine::DeckId deck) { return deckGrid(deck); };
    automix.pickNext = [this](int current, const std::vector<int>& remaining) { return pickAutomixNext(current, remaining); };
    automixTimer = std::make_unique<juce::TimedCallback>(
        [this]
        {
            for (const auto& c : automix.tick(engine.readState()))
            {
                const auto* start = std::get_if<engine::StartTransition>(&c);
                if (start != nullptr && start->curves != nullptr) // a saved / Auto transition (P4-19)
                    engine.startTransition(*start, automix.takeCurves());
                else
                    engine.send(c);
            }
            // The order played, in the log (to compare smart with plain order).
            if (const auto& order = automix.playedOrder(); static_cast<int>(order.size()) > automixLogged)
            {
                for (; automixLogged < static_cast<int>(order.size()); ++automixLogged)
                {
                    const int i = order[static_cast<std::size_t>(automixLogged)];
                    util::logInfo("Automix{} #{}: queue item {} ({})", automix.getSettings().smart ? " smart" : "",
                                  automixLogged + 1, i + 1,
                                  i >= 0 && i < static_cast<int>(automixNames.size())
                                      ? automixNames[static_cast<std::size_t>(i)].toStdString()
                                      : std::string("?"));
                }
            }
            const int now = automix.running() ? automix.playingIndex() : -1;
            const int next = automix.running() ? automix.nextIndex() : -1;
            const auto name = [this](int i)
            { return i >= 0 && i < static_cast<int>(automixNames.size()) ? automixNames[static_cast<std::size_t>(i)] : juce::String("-"); };
            juce::String text = automixFiles.empty() ? juce::String("Queue some tracks: select them in the browser, then QUEUE.")
                                                     : juce::String(automixFiles.size()) + " tracks queued";
            if (automix.running())
                text << "   Now: " << name(now) << "   Next: " << name(next);
            automixPanel.setStatus(automix.running(), text);
        });
    automixTimer->startTimerHz(10);
    // Track Matcher (P3-19).
    if (libraryDb != nullptr)
    {
        matcherService = std::make_unique<matcher::MatcherService>(*libraryDb);
        matcherService->onResult = [safe = juce::Component::SafePointer<MainComponent>(this)](const matcher::MatchResult& r)
        {
            juce::MessageManager::callAsync(
                [safe, r]
                {
                    if (safe == nullptr)
                        return;
                    safe->suggestions = r.suggestions;
                    safe->suggestionsFor = r.contextTrackId;
                    safe->suggestionHighlight = 0;
                    safe->showSuggestions();
                    util::logInfo("Matcher: {} suggestions from {} candidates in {:.1f} ms", r.suggestions.size(),
                                  r.candidates, r.milliseconds);
                });
        };
        matcherTimer = std::make_unique<juce::TimedCallback>([this] { askMatcher(); });
        matcherTimer->startTimerHz(10);
    }
    // The Next Track panel (P3-24).
    addAndMakeVisible(nextTrackPanel);
    // The Beat Maker (P4-06): the last project, else the Club kit on pattern A.
    {
        auto loaded = beatmaker::loadProject(beatProjectFile().getFullPathName().toWideCharPointer());
        if (loaded)
            beatProject = std::move(loaded).value();
        else
        {
            beatProject = beatmaker::defaultProject();
            beatProject.patterns[0].drums = beatmaker::kitTracks(beatmaker::drumKits()[0]);
        }
    }
    addChildComponent(beatPanel);
    beatPanel.setProject(&beatProject);
    beatSaveTimer = std::make_unique<juce::TimedCallback>(
        [this]
        {
            beatSaveTimer->stopTimer();
            if (const auto r = beatmaker::saveProject(beatProject, beatProjectFile().getFullPathName().toWideCharPointer()); !r)
                util::logWarning("Beat Maker: {}", r.error());
        });
    // The producer (P4-10): its own thread, pushing into the engine's Beat Maker queue.
    beatProducer = std::make_unique<app::BeatMakerProducer>(
        [this](const engine::ScheduledEvent& e) { return engine.beatMakerEvents().tryPush(e); });
    beatPanel.onChanged = [this]
    {
        beatSaveTimer->startTimer(1000);
        beatProjectChanged();
    };
    beatPanel.onPlayStop = [this]
    {
        if (beatPlaying)
            stopBeatMaker();
        else
            startBeatMaker();
    };
    beatPanel.onPatternShown = [this](int p)
    {
        if (beatPlaying && !beatSongMode)
            beatProducer->queuePattern(p, beatSwitchAtEnd);
    };
    beatPanel.grid().onSongMode = [this](bool on)
    {
        beatSongMode = on;
        if (beatPlaying)
            beatProducer->queueSong(on, beatPanel.grid().pattern(), beatSwitchAtEnd);
    };
    beatPanel.grid().onSwitchAt = [this](bool atEnd) { beatSwitchAtEnd = atEnd; };
    beatPanel.grid().onHit = [this](int track) { beatHit(track); };
    beatPanel.grid().onExport = [this] { showBeatExportMenu(); };
    // User samples (P4-13).
    samplesFolder().createDirectory();
    beatPanel.grid().isSampleMissing = [this](const std::string& path) { return sampleBank.isMissing(path); };
    beatPanel.grid().onSampleFile = [this](int track, const juce::File& file) { setBeatSample(track, file); };
    beatPanel.grid().onChooseSample = [this](int track) { chooseBeatSample(track); };
    addChildComponent(beatStrip);
    beatStrip.setTempo(beatProject.followMaster, beatProject.ownBpm);
    beatStrip.onTempo = [this](bool follow, double bpm)
    {
        beatPanel.edit([&](beatmaker::Project& p)
                       {
                           p.followMaster = follow;
                           p.ownBpm = bpm;
                       });
    };
    beatProjectChanged();
    // The transition editor (P4-17).
    addChildComponent(mixEditor);
    loadMixPresetNames();
    mixEditor.onChanged = [this](const ui::TransitionEditor::Edit& e)
    {
        if (mixPlan)
        {
            mixPlan->edit = e;
            mixSaveTimer->startTimer(600); // saved for this pair a moment after the last edit (P4-19)
        }
    };
    mixSaveTimer = std::make_unique<juce::TimedCallback>(
        [this]
        {
            mixSaveTimer->stopTimer();
            saveMixPlan();
        });
    mixEditor.onAuto = [this]
    {
        // Back to Auto: the pair's saved one goes.
        if (mixPlan && libraryDb != nullptr && mixPlan->fromTrack != 0 && mixPlan->toTrack != 0)
        {
            const auto from = mixPlan->fromTrack, to = mixPlan->toTrack;
            libraryDb->write([from, to](library::Connection& c) { app::deleteTransition(c, from, to); });
            transitionLabels.erase(from, to);
            libraryBrowser->repaintRows();
        }
        mixPlan.reset();
        refreshMix(true);
    };
    if (gridReader != nullptr)
        transitionLabels.load(*gridReader);
    libraryBrowser->transitionLabel = [this](std::int64_t a, std::int64_t b) { return transitionLabels.get(a, b); };
    // Automix plays the pair's saved transition, else Auto (P4-19).
    automix.transitionFor = [this](engine::DeckId from, engine::DeckId) -> std::optional<app::Automix::PlannedTransition>
    {
        const auto e = transitionFor(from);
        if (!e)
            return std::nullopt;
        return app::Automix::PlannedTransition{std::make_shared<const engine::TransitionCurves>(e->curves), e->outBeat, e->inBeat};
    };
    mixEditor.onPreset = [this](int index)
    {
        if (!mixPlan)
            return;
        auto e = mixPlan->edit;
        const float beats = e.curves.beats;
        if (index < app::kTransitionPresetCount)
        {
            const auto p = static_cast<app::TransitionPreset>(index);
            e.curves = app::makePreset(p, beats);
            e.preset = app::presetName(p);
        }
        else
        {
            const auto user = app::loadUserPresets();
            const auto u = static_cast<std::size_t>(index - app::kTransitionPresetCount);
            if (u >= user.size())
                return;
            e.curves = app::scaled(user[u].shape, beats);
            e.preset = user[u].name;
        }
        e.reasons.clear();
        mixPlan->edit = e;
        mixEditor.setEdit(e);
    };
    mixEditor.onSavePreset = [this](const juce::String& name)
    {
        if (!mixPlan)
            return;
        if (const auto error = app::saveUserPreset(mixPlan->edit.curves, name); error.isNotEmpty())
            libraryBrowser->setStatus("Transition preset: " + error);
        loadMixPresetNames();
    };
    mixEditor.onPreview = [this](const ui::TransitionEditor::Edit& e)
    {
        if (mixPlan)
            startPlannedTransition(e, mixPlan->from, true);
    };
    beatTimer = std::make_unique<juce::TimedCallback>(
        [this]
        {
            // The transition editor follows the decks: new tracks, a new plan (P4-17).
            if (mixEditor.isVisible())
            {
                const auto& st = engine.readState();
                if (!mixPlan || st.decks[mixPlan->from].bufferId != mixPlan->fromBuffer ||
                    st.decks[1 - mixPlan->from].bufferId != mixPlan->toBuffer)
                    refreshMix(false);
            }
            // The producer's clock: the engine's sample clock, master beat and tempo, and when (P4-10).
            const auto& es = engine.readState();
            beatProducer->setClock({es.sampleClock, es.masterBeat, es.masterBpm, es.sampleRate, es.hostTimeSeconds});
            const auto onPads = [this](ui::PadMode m) { return padModes[0] == m || padModes[1] == m; };
            if (!beatPanel.isVisible() && !onPads(ui::PadMode::BeatPatterns) && !onPads(ui::PadMode::BeatPads))
                return;
            beatStrip.refresh();
            // What plays now: the shown pattern's step lit, the playing / queued patterns marked.
            const auto status = beatProducer->status();
            const auto pos = status.playing ? status.position : beatmaker::Sequencer::Position{};
            const int queuedPattern = status.playing ? status.queued : -1;
            beatPanel.grid().setPlayback(beatPlaying, pos.pattern == beatPanel.grid().pattern() ? pos.step : -1);
            beatPanel.grid().setPatternStates(pos.pattern, queuedPattern);
            // The pads: the playing (else shown) pattern's drum tracks.
            std::array<juce::String, 8> padNames;
            const int padPattern = pos.pattern >= 0 ? pos.pattern : beatPanel.grid().pattern();
            const auto& drums = beatProject.patterns[static_cast<std::size_t>(padPattern)].drums;
            for (std::size_t i = 0; i < 8 && i < drums.size(); ++i)
                padNames[i] = juce::String::fromUTF8(drums[i].name.c_str());
            for (auto* deck : {&deckA, &deckB})
            {
                deck->setBeatPatternLooks(pos.pattern, queuedPattern);
                deck->setBeatPadNames(padNames, beatPanel.grid().recording());
            }
        });
    beatTimer->startTimerHz(30);
    {
        auto s = nextTrackPanel.state();
        for (const auto& p : matcherPresets)
            s.presets.add(juce::String(p.name));
        nextTrackPanel.setState(s);
    }
    const auto changed = [this]
    {
        showSuggestions();
        askMatcher(true);
    };
    nextTrackPanel.onIntent = [this, changed](int i) { matcherIntent = i; changed(); };
    nextTrackPanel.onKeyPolicy = [this, changed](int i) { matcherKeyPolicy = i; changed(); };
    nextTrackPanel.onScope = [this, changed](int i) { matcherScope = i; changed(); };
    nextTrackPanel.onPreset = [this, changed](int i) { matcherPreset = i; changed(); };
    nextTrackPanel.onHighlight = [this](int row)
    {
        suggestionHighlight = row;
        showSuggestions();
    };
    nextTrackPanel.onLoad = [this](int row)
    {
        suggestionHighlight = row;
        loadSuggestion();
    };
    {
    }
    for (auto* tab : {&fxTab, &samplerTab, &recordTab, &automixTab})
    {
        tab->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1f1f26));
        tab->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff3a3a46));
        tab->setWantsKeyboardFocus(false);
        addAndMakeVisible(*tab);
    }
    fxTab.onClick = [this] { showPanel(0); };
    samplerTab.onClick = [this] { showPanel(1); };
    recordTab.onClick = [this] { showPanel(2); };
    automixTab.onClick = [this] { showPanel(3); };
    showPanel(0);
    samplerPanel.onLoadFile = [this](int slot) { chooseSampleFile(slot); };
    samplerPanel.onLoadSelected = [this](int slot)
    {
        const auto file = libraryBrowser->selectedTrackFile();
        if (file == juce::File())
            return;
        double bpm = 0.0;
        juce::String name = file.getFileNameWithoutExtension();
        if (const auto id = libraryBrowser->selectedTrackId(); id != 0 && gridReader != nullptr)
            if (const auto t = library::TrackRepository(*gridReader).get(id))
            {
                bpm = t->bpm.value_or(0.0);
                if (!t->title.empty())
                    name = juce::String::fromUTF8(t->title.c_str());
            }
        loadSample(slot, file, bpm, name);
    };
    samplerPanel.onClear = [this](int slot)
    {
        engine.send(engine::SamplerLoad{static_cast<std::uint8_t>(slot), nullptr, 0, 0.0f});
        samplerPanel.setSlotName(slot, {});
    };
    samplerPanel.onBank = [this](int bank)
    {
        deckA.setSamplerBank(bank);
        deckB.setSamplerBank(bank);
        showSequenceLooks();
    };
    mixer.onColorFx = [this](int type)
    {
        if (onColorFxChanged)
            onColorFxChanged(type);
    };
    {
        using T = dsp::fx::EffectType;
        const std::array<std::array<T, 3>, 2> start{{{T::Echo, T::Reverb, T::Flanger}, {T::Roll, T::Trans, T::FilterLfo}}};
        for (int u = 0; u < 2; ++u)
        {
            engine.send(engine::FxCommand{static_cast<std::uint8_t>(u), engine::FxOp::Target, 0, 0,
                                          static_cast<float>(u == 0 ? engine::FxTarget::Deck0 : engine::FxTarget::Deck1)});
            for (int s = 0; s < 3; ++s)
            {
                engine.send(engine::FxCommand{static_cast<std::uint8_t>(u), engine::FxOp::Wet,
                                              static_cast<std::uint8_t>(s), 0, 0.5f});
                chooseEffect(u, s, static_cast<int>(start[static_cast<std::size_t>(u)][static_cast<std::size_t>(s)]));
            }
        }
    }
    installPadFxBank(); // P3-12
    samplerPanel.onSequence = [this](int index) { callSequence(samplerPanel.bank(), index, false); };
    fxTimer = std::make_unique<juce::TimedCallback>(
        [this]
        {
            engine.collectEffects();
            keepSequence(); // P3-15
        });
    fxTimer->startTimerHz(2);
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
        view.onHotCuePad = [this, d](int pad, bool down) { padAction(d, pad, false, !down); };
        view.onPadModeMenu = [this, d] { showPadModeMenu(d); };
        view.onPadFxMenu = [this, d](int pad) { showPadFxMenu(d, pad); };
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
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MainComponent>(this)]
                                        { if (safe != nullptr) safe->askMatcher(true); }); // P3-24: at once
        if (deck < 2 && automixLoading[deck] >= 0) // Automix's next track is on the deck (with its grid)
        {
            automix.loaded(deck, automixLoading[deck]);
            automixLoading[deck] = -1;
        }
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
    // The keymap editor waiting for a key (P3-20): it gets the next one (modifiers alone don't count).
    if (keymapEditor != nullptr && keymapEditor->isCapturing())
    {
        if (e.down && !e.repeat && !ui::scan::isModifier(e.scanCode))
            keymapEditor->captureChord({e.scanCode, e.modifiers});
        return true;
    }
    // Only while the main window has the keyboard (dialogs and file choosers get their keys untouched).
    auto* peer = getPeer();
    if (peer == nullptr || !peer->isFocused())
    {
        keyboard.releaseAll();
        return false;
    }
    const auto* focused = juce::Component::getCurrentlyFocusedComponent();
    const bool typing = dynamic_cast<const juce::TextInputTarget*>(focused) != nullptr;
    // The Beat Maker's keys while its grid has the keyboard (P4-06).
    const bool inBeatMaker = beatPanel.isVisible() && focused != nullptr &&
                             (focused == &beatPanel || beatPanel.isParentOf(focused));
    const bool inMix = mixEditor.isVisible() && focused != nullptr && (focused == &mixEditor || mixEditor.isParentOf(focused));
    return keyboard.handle(e, inBeatMaker ? ui::KeyContext::BeatMaker : inMix ? ui::KeyContext::Transition : ui::KeyContext::Main,
                           typing);
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
    // The Beat Maker's grid (P4-06): every Beat Maker action goes to it (held keys repeat where they may).
    for (int a = static_cast<int>(GA::BeatMakerStartStop); a <= static_cast<int>(GA::BeatMakerLoadSample); ++a)
        if (a < static_cast<int>(GA::CodeEvaluate) || a > static_cast<int>(GA::BrowserBigger))
            actions.setHandler(ui::globalCommand(static_cast<GA>(a)),
                               [this, a](Phase p)
                               {
                                   if (p != Phase::Release)
                                       beatPanel.perform(static_cast<GA>(a));
                               });
    onPress(ui::globalCommand(GA::SwitchPanel), [this] { switchBottomPanel(); });
    // The transition editor's keys (P4-17).
    for (int a = static_cast<int>(GA::TransLane1); a <= static_cast<int>(GA::TransLoop); ++a)
        actions.setHandler(ui::globalCommand(static_cast<GA>(a)),
                           [this, a](Phase p)
                           {
                               if (p != Phase::Release)
                                   mixEditor.perform(static_cast<GA>(a));
                           });

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
        // Pads (P3-01, P3-06): pad 1-8 in the deck's pad mode; Shift + pad is the alternate (hot cue: delete).
        for (int pad = 0; pad < 8; ++pad)
        {
            actions.setHandler(cmd(static_cast<DA>(static_cast<int>(DA::Pad1) + pad)),
                               [this, d, pad](Phase p)
                               {
                                   if (p == Phase::Repeat)
                                       return;
                                   (d == 0 ? deckA : deckB).setPadHeld(pad, p == Phase::Press);
                                   padAction(d, pad, false, p == Phase::Release);
                               });
            onPress(cmd(static_cast<DA>(static_cast<int>(DA::PadAlt1) + pad)),
                    [this, d, pad] { padAction(d, pad, true, false); });
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
        onPress(cmd(DA::CaptureToSampler), // P3-14 (no default key): the loop, else the slicer range
                [this, d] { captureToSampler(d, !engine.readState().decks[d].loopActive); });
        // Release FX (P3-11), held: no default keys (bind them in keymap.json); the FX panel has buttons.
        for (const auto [action, kind] : {std::pair{DA::ReleaseEchoOut, 0}, std::pair{DA::ReleaseBrake, 1},
                                          std::pair{DA::ReleaseSpinBack, 2}, std::pair{DA::ReleaseSweep, 3}})
            actions.setHandler(cmd(action),
                               [this, d, kind = kind](Phase p)
                               {
                                   if (p == Phase::Repeat)
                                       return;
                                   // The beat length of the FX unit on this deck (else 1 beat).
                                   float beats = 1.0f;
                                   const auto& s = engine.readState();
                                   for (const auto& u : s.fx)
                                       if (static_cast<int>(u.target) == static_cast<int>(d))
                                           beats = u.slots[0].beats;
                                   engine.send(engine::ReleaseFxCommand{d, static_cast<std::uint8_t>(kind),
                                                                        p == Phase::Press, beats});
                               });
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
    onPress(ui::globalCommand(GA::Record), [this] { toggleRecording(); }); // F9 (P3-16)
    onPress(ui::globalCommand(GA::Automix), [this] { toggleAutomix(); });   // F10 (P3-18)
    onPress(ui::globalCommand(GA::LoadSuggestion), [this] { loadSuggestion(); }); // N (P3-19)
    onPress(ui::globalCommand(GA::NextSuggestion),
            [this]
            {
                if (!suggestions.empty())
                    suggestionHighlight = (suggestionHighlight + 1) % static_cast<int>(suggestions.size());
                showSuggestions();
            });
    actions.setHandler(ui::globalCommand(GA::PreviewSuggestion),
                       [this](Phase p)
                       {
                           if (p != Phase::Repeat)
                               previewSuggestion(p == Phase::Press);
                       });
    const auto matcherChanged = [this]
    {
        showSuggestions();
        askMatcher(true);
    };
    onPress(ui::globalCommand(GA::CycleIntent), [this, matcherChanged] { matcherIntent = (matcherIntent + 1) % 4; matcherChanged(); });
    onPress(ui::globalCommand(GA::MatcherPreset),
            [this, matcherChanged] { matcherPreset = (matcherPreset + 1) % static_cast<int>(matcherPresets.size()); matcherChanged(); });
    onPress(ui::globalCommand(GA::MatcherKeyPolicy), [this, matcherChanged] { matcherKeyPolicy = (matcherKeyPolicy + 1) % 3; matcherChanged(); });
    onPress(ui::globalCommand(GA::MatcherScope), [this, matcherChanged] { matcherScope = 1 - matcherScope; matcherChanged(); });
    onPress(ui::globalCommand(GA::AutoTransition), [this] { autoTransition(); }); // T (P3-17)
    onPress(ui::globalCommand(GA::AutoTransitionCancel), [this] { engine.send(engine::CancelTransition{}); });
    // FX (P3-09): F5 / F7 the unit on / off, F6 / F8 slot 1's next effect, Shift+F5 / Shift+F7 the target.
    onPress(ui::globalCommand(GA::Fx1Toggle), [this] { fxPanel.toggleUnit(0); });
    onPress(ui::globalCommand(GA::Fx2Toggle), [this] { fxPanel.toggleUnit(1); });
    onPress(ui::globalCommand(GA::Fx1Next), [this] { fxPanel.nextEffect(0); });
    onPress(ui::globalCommand(GA::Fx2Next), [this] { fxPanel.nextEffect(1); });
    onPress(ui::globalCommand(GA::Fx1Target), [this] { fxPanel.cycleTarget(0); });
    onPress(ui::globalCommand(GA::Fx2Target), [this] { fxPanel.cycleTarget(1); });
    // Pad modes (P3-06), both decks: F1-F4, Shift+F1-F4. F1 in hot cue mode switches A-H / I-P.
    onPress(ui::globalCommand(GA::PadModeHotCue),
            [this]
            {
                if (padModes[0] == ui::PadMode::HotCue && padModes[1] == ui::PadMode::HotCue)
                    setHotCuePage(1 - hotCuePage);
                for (engine::DeckId d = 0; d < 2; ++d)
                    setPadMode(d, ui::PadMode::HotCue);
            });
    for (const auto [action, mode] : {std::pair{GA::PadModeBeatLoop, ui::PadMode::BeatLoop},
                                      std::pair{GA::PadModeBeatJump, ui::PadMode::BeatJump},
                                      std::pair{GA::PadModeSlicer, ui::PadMode::Slicer},
                                      std::pair{GA::PadModeKeyShift, ui::PadMode::KeyShift},
                                      std::pair{GA::PadModeKeyboard, ui::PadMode::Keyboard},
                                      std::pair{GA::PadModePadFx, ui::PadMode::PadFx},
                                      std::pair{GA::PadModeSampler, ui::PadMode::Sampler}})
        onPress(ui::globalCommand(action),
                [this, mode = mode]
                {
                    for (engine::DeckId d = 0; d < 2; ++d)
                        setPadMode(d, mode);
                });
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

void MainComponent::setColorFx(int type)
{
    engine.send(engine::SetColorFx{static_cast<std::uint8_t>(std::clamp(type, 0, dsp::fx::ColorFx::kTypeCount - 1))});
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
    m.addItem("Capture the loop to the sampler", s.loopActive, false, [this, deck] { captureToSampler(deck, false); });
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

void MainComponent::chooseEffect(int unit, int slot, int type)
{
    auto effect = type >= 0 ? dsp::fx::makeEffect(static_cast<dsp::fx::EffectType>(type)) : nullptr;
    if (!engine.installEffect(unit, slot, std::move(effect)))
        return;
    fxPanel.setEffectType(unit, slot, type);
    // The unit's beat length for the new effect too.
    engine.send(engine::FxCommand{static_cast<std::uint8_t>(unit), engine::FxOp::Beats, static_cast<std::uint8_t>(slot),
                                  0, engine.readState().fx[static_cast<std::size_t>(unit)].slots[static_cast<std::size_t>(slot)].beats});
}

void MainComponent::setPadMode(engine::DeckId deck, ui::PadMode mode)
{
    if (deck >= 2)
        return;
    if ((padModes[deck] == ui::PadMode::Slicer) != (mode == ui::PadMode::Slicer)) // the slicer domain (P3-07)
        engine.send(engine::SetSlicer{deck, mode == ui::PadMode::Slicer, slicerBeats[deck]});
    padModes[deck] = mode;
    (deck == 0 ? deckA : deckB).setPadMode(mode);
}

void MainComponent::padAction(engine::DeckId deck, int pad, bool alternate, bool release)
{
    if (deck >= 2)
        return;
    const auto& state = engine.readState().decks[deck];
    if (padModes[deck] == ui::PadMode::BeatPads) // P4-11: the Beat Maker's drum tracks
    {
        if (!release && !alternate)
            beatHit(pad);
        return;
    }
    if (padModes[deck] == ui::PadMode::BeatPatterns) // P4-09: the Beat Maker's patterns
    {
        if (release || alternate)
            return;
        beatPanel.grid().perform(static_cast<ui::GlobalAction>(static_cast<int>(ui::GlobalAction::BeatMakerPattern1) + pad));
        if (!beatPlaying)
            startBeatMaker();
        return;
    }
    if (padModes[deck] == ui::PadMode::SequenceCall) // P3-15: the bank's sequences
    {
        if (release || alternate)
            return;
        const int bank = samplerPanel.bank();
        if (bank == sequenceBank && pad == sequenceIndex && engine.readState().sequencer.playing)
            engine.send(engine::SequencerCommand{engine::SequencerOp::Play, 0, 0.0f}); // the playing one: stop
        else
            callSequence(bank, pad, true);
        return;
    }
    if (padModes[deck] == ui::PadMode::Sampler) // P3-13: the sampler's bank
    {
        if (alternate)
        {
            if (!release && pad < engine::Sampler::kBanks) // Shift + pad 1-4: the bank
                samplerPanel.setBank(pad);
            return;
        }
        engine.send(engine::SamplerPad{
            static_cast<std::uint8_t>(samplerPanel.bank() * engine::Sampler::kSlotsPerBank + pad), release});
        return;
    }
    if (padModes[deck] == ui::PadMode::PadFx) // P3-12: the presets
    {
        if (alternate)
        {
            if (!release && pad < app::kPadFxBanks && pad != padFxBank) // Shift + pad 1 / 2: the bank
            {
                padFxBank = pad;
                installPadFxBank();
            }
            return;
        }
        const auto& preset = padFxPresets[static_cast<std::size_t>(padFxBank)][static_cast<std::size_t>(pad)];
        for (const auto& c : app::padFxCommands(preset, deck, pad, release, state))
            engine.send(c);
        return;
    }
    for (const auto& c : ui::padCommands(padModes[deck], deck, pad, hotCuePage, alternate, release, state))
    {
        if (const auto* slicer = std::get_if<engine::SetSlicer>(&c))
            slicerBeats[deck] = slicer->beats;
        engine.send(c);
    }
}

juce::File MainComponent::samplesFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Devil-DJ/Samples");
}

void MainComponent::captureToSampler(engine::DeckId deck, bool slicer)
{
    if (deck >= 2)
        return;
    const auto& s = engine.readState();
    const auto& st = s.decks[deck];
    if (slicer ? !st.slicerOn : !st.loopActive)
        return;
    const double a = slicer ? st.slicerStart : st.loopStart;
    const double b = slicer ? st.slicerEnd : st.loopEnd;
    const auto track = registry.get(st.bufferId);
    if (track == nullptr)
        return;
    std::shared_ptr<const engine::TrackBuffer> copy =
        app::copyRange(*track, static_cast<int>(std::llround(a)), static_cast<int>(std::llround(b)));
    if (copy == nullptr)
        return;
    const auto& grid = deckGrids[deck].grid;
    const double bpm = grid.empty() ? 0.0 : grid.bpmAt(a);
    // The shown bank's first empty slot, else any empty one, else the bank's first.
    const int bank = samplerPanel.bank() * engine::Sampler::kSlotsPerBank;
    int slot = -1;
    for (int i = 0; i < engine::Sampler::kSlots && slot < 0; ++i)
    {
        const int candidate = (bank + i) % engine::Sampler::kSlots;
        if (!s.sampler[static_cast<std::size_t>(candidate)].loaded)
            slot = candidate;
    }
    if (slot < 0)
        slot = bank;
    const auto* raw = copy.get();
    const auto id = registry.add(copy);
    if (!engine.send(engine::SamplerLoad{static_cast<std::uint8_t>(slot), raw, id, static_cast<float>(bpm)}))
    {
        registry.release(id);
        return;
    }
    const auto title = track->title.isNotEmpty() ? track->title : track->file.getFileNameWithoutExtension();
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H%M%S");
    const auto name = juce::String(slicer ? "Slices " : "Loop ") + (bpm > 0.0 ? juce::String(bpm, 1) + " " : juce::String()) + title;
    samplerPanel.setSlotName(slot, name);
    // Saved for later (and for other software), off the message thread.
    const auto file = samplesFolder().getChildFile(juce::File::createLegalFileName(name + " " + stamp + ".wav"));
    juce::Thread::launch(
        [copy, file]
        {
            file.getParentDirectory().createDirectory();
            if (const auto error = app::writeLoopWav(*copy, 0, copy->numSamples(), file, 32); error.isNotEmpty())
                util::logWarning("Capture: {}", error.toStdString());
            else
                util::logInfo("Capture: saved {}", file.getFullPathName().toStdString());
        });
}

void MainComponent::setSequences(const juce::String& json)
{
    sequences = app::sequencesFromJson(json);
    callSequence(0, 0, false);
}

void MainComponent::keepSequence()
{
    const auto& s = engine.readState().sequencer;
    if (s.recording)
        return; // when recording stops
    auto now = app::fromEngine(s.sequence);
    auto& stored = sequences[static_cast<std::size_t>(sequenceBank)][static_cast<std::size_t>(sequenceIndex)];
    const auto same = [](const app::StoredSequence& a, const app::StoredSequence& b)
    {
        if (a.bars != b.bars || a.events.size() != b.events.size())
            return false;
        for (std::size_t i = 0; i < a.events.size(); ++i)
            if (a.events[i].beat != b.events[i].beat || a.events[i].slot != b.events[i].slot ||
                a.events[i].kind != b.events[i].kind)
                return false;
        return true;
    };
    if (same(now, app::fromEngine([&] {
            engine::Sequence copy;
            copy.bars = static_cast<std::uint8_t>(stored.bars);
            copy.count = static_cast<int>(stored.events.size());
            for (std::size_t i = 0; i < stored.events.size() && i < copy.events.size(); ++i)
                copy.events[i] = stored.events[i];
            return copy;
        }())))
        return;
    stored = std::move(now);
    showSequenceLooks();
    if (onSequencesChanged)
        onSequencesChanged(app::toJson(sequences));
}

void MainComponent::callSequence(int bank, int index, bool play)
{
    keepSequence(); // the one in the engine first
    sequenceBank = std::clamp(bank, 0, app::kSequenceBanks - 1);
    sequenceIndex = std::clamp(index, 0, app::kSequencesPerBank - 1);
    if (engine.readState().sequencer.recording)
        engine.send(engine::SequencerCommand{engine::SequencerOp::Record, 0, 0.0f});
    for (const auto& c :
         app::loadCommands(sequences[static_cast<std::size_t>(sequenceBank)][static_cast<std::size_t>(sequenceIndex)]))
        engine.send(c);
    if (play)
        engine.send(engine::SequencerCommand{engine::SequencerOp::Play, 0, 1.0f});
    samplerPanel.setSequenceIndex(sequenceIndex);
    showSequenceLooks();
}

void MainComponent::showSequenceLooks()
{
    std::array<bool, 8> filled{};
    const auto& bank = sequences[static_cast<std::size_t>(samplerPanel.bank())];
    for (std::size_t i = 0; i < filled.size(); ++i)
        filled[i] = !bank[i].empty();
    const int current = samplerPanel.bank() == sequenceBank ? sequenceIndex : -1;
    deckA.setSequenceLooks(filled, current);
    deckB.setSequenceLooks(filled, current);
}

void MainComponent::showPanel(int which)
{
    shownPanel = std::clamp(which, 0, 3);
    fxTab.setToggleState(shownPanel == 0, juce::dontSendNotification);
    samplerTab.setToggleState(shownPanel == 1, juce::dontSendNotification);
    recordTab.setToggleState(shownPanel == 2, juce::dontSendNotification);
    automixTab.setToggleState(shownPanel == 3, juce::dontSendNotification);
    resized();
}

void MainComponent::autoTransition()
{
    const auto& s = engine.readState();
    if (s.transition.active)
    {
        engine.send(engine::CancelTransition{});
        return;
    }
    // From the deck leading the clock (else the one playing) to the other.
    int from = s.masterSource >= 0 && s.masterSource < 2 ? s.masterSource : -1;
    if (from < 0)
        from = s.decks[0].playing ? 0 : s.decks[1].playing ? 1 : -1;
    if (from < 0 || !s.decks[1 - from].loaded)
        return;
    // The transition edited in the MIX panel, if it is for these two tracks (P4-17); else the pair's saved one, else
    // Auto (P4-19); else (not analyzed) the style chosen in the Mixer menu.
    if (mixPlan && mixPlan->from == from && mixPlan->fromBuffer == s.decks[from].bufferId &&
        mixPlan->toBuffer == s.decks[1 - from].bufferId)
    {
        startPlannedTransition(mixPlan->edit, from, false);
        return;
    }
    if (const auto planned = transitionFor(from))
    {
        startPlannedTransition(*planned, from, false);
        return;
    }
    const auto style = transitionStyle == 1   ? engine::TransitionStyle::FilterSweep
                       : transitionStyle == 2 ? engine::TransitionStyle::EchoOut
                                              : engine::TransitionStyle::Blend;
    engine.send(engine::StartTransition{static_cast<engine::DeckId>(from), static_cast<engine::DeckId>(1 - from),
                                        static_cast<float>(transitionBeats), style, -1.0});
}

void MainComponent::askMatcher(bool now)
{
    if (matcherService == nullptr)
        return;
    const auto& s = engine.readState();
    int master = s.masterSource >= 0 && s.masterSource < 2 ? s.masterSource : -1;
    if (master < 0)
        master = s.decks[0].playing ? 0 : s.decks[1].playing ? 1 : -1;
    matcher::MatcherTriggers::Snapshot snap;
    snap.masterDeck = master;
    snap.trackIds = {deckTrackId(0), deckTrackId(1)};
    if (master >= 0 && s.decks[static_cast<std::size_t>(master)].lengthSamples > 0)
        snap.masterFraction = s.decks[static_cast<std::size_t>(master)].positionSamples /
                              s.decks[static_cast<std::size_t>(master)].lengthSamples;
    snap.analysisVersion = analysisRuns;
    const bool fire = matcherTriggers.update(snap);
    if ((!fire && !now) || master < 0 || deckTrackId(static_cast<engine::DeckId>(master)) == 0)
        return;
    // The energy line: the master tracks so far (the last 5).
    if (const auto id = deckTrackId(static_cast<engine::DeckId>(master)); id != lastMasterTrack)
    {
        lastMasterTrack = id;
        if (const auto e = energyOf(id))
            energyHistory.push_back(*e);
        if (energyHistory.size() > 5)
            energyHistory.erase(energyHistory.begin());
    }
    const auto& a = s.decks[static_cast<std::size_t>(master)];
    matcher::MatchRequest r;
    r.contextTrackId = deckTrackId(static_cast<engine::DeckId>(master));
    r.aBpmNow = a.gridBpm > 0.0 ? a.gridBpm * (1.0 + a.tempoPercent / 100.0) : 0.0;
    r.keyLockOff = !s.decks[static_cast<std::size_t>(1 - master)].keyLock;
    r.nowUnixSeconds = juce::Time::currentTimeMillis() / 1000;
    r.playedThisSession = {deckTrackId(0), deckTrackId(1)};
    r.intent = matcherIntent == 0 ? matcher::Intent::Down : matcherIntent == 2 ? matcher::Intent::Up
              : matcherIntent == 3                       ? matcher::Intent::Switch
                                                         : matcher::Intent::Hold;
    r.keyPolicy = matcherKeyPolicy == 0 ? matcher::KeyPolicy::Strict : matcherKeyPolicy == 2 ? matcher::KeyPolicy::Off
                                                                                           : matcher::KeyPolicy::Relaxed;
    r.scope.playlistId = matcherScope == 1 ? libraryBrowser->shownPlaylist() : 0;
    r.preset = matcherPresets[static_cast<std::size_t>(std::clamp(matcherPreset, 0, static_cast<int>(matcherPresets.size()) - 1))].name;
    matcherService->request(std::move(r));
}

std::optional<double> MainComponent::energyOf(std::int64_t trackId)
{
    if (gridReader == nullptr)
        return std::nullopt;
    if (auto* st = gridReader->prepare("SELECT energy FROM track_features WHERE track_id = ?"))
    {
        st->bind(1, trackId);
        std::optional<double> e;
        if (st->step())
            e = st->getOptionalDouble(0);
        st->reset();
        return e;
    }
    return std::nullopt;
}

void MainComponent::startBeatMaker()
{
    // From the next bar of its timeline: the song (song mode) or the pattern shown.
    const auto& es = engine.readState();
    beatProducer->setClock({es.sampleClock, es.masterBeat, es.masterBpm, es.sampleRate, es.hostTimeSeconds});
    beatProducer->play(beatPanel.grid().pattern(), beatSongMode);
    beatPlaying = true;
}

void MainComponent::beatHit(int track)
{
    const auto at = beatProducer->hit(beatPanel.grid().pattern(), track, 100);
    if (at && beatPlaying && beatPanel.grid().recording())
        beatPanel.edit([&](beatmaker::Project& p) { beatmaker::recordHit(p, *at, track, 100); });
}

juce::File MainComponent::beatRendersFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Devil-DJ")
        .getChildFile("Beats")
        .getChildFile("Renders");
}

void MainComponent::showBeatExportMenu()
{
    const int p = beatPanel.grid().pattern();
    const auto letter = juce::String::charToString(static_cast<juce::juce_wchar>('A' + p));
    const bool song = !beatProject.song.empty();
    juce::PopupMenu m;
    m.addSectionHeader("Export");
    m.addItem("Pattern " + letter + " to a WAV file...", [this] { exportBeat(BeatExport::PatternWav); });
    m.addItem("Pattern " + letter + " x 4 to a WAV file...", [this] { exportBeat(BeatExport::Pattern4Wav); });
    m.addItem("The song to a WAV file...", song, false, [this] { exportBeat(BeatExport::SongWav); });
    m.addSeparator();
    m.addItem("Pattern " + letter + " to the sampler (a loop)", [this] { exportBeat(BeatExport::PatternSampler); });
    m.addItem("Pattern " + letter + " to the library", [this] { exportBeat(BeatExport::PatternLibrary); });
    m.addItem("The song to the library", song, false, [this] { exportBeat(BeatExport::SongLibrary); });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&beatPanel.grid().exportButton()));
}

void MainComponent::exportBeat(BeatExport what)
{
    // Rendered on a worker thread from a copy of the project (a song can take a moment), then handed back here.
    const auto& es = engine.readState();
    app::BeatRenderOptions o;
    o.pattern = beatPanel.grid().pattern();
    o.song = what == BeatExport::SongWav || what == BeatExport::SongLibrary;
    o.passes = what == BeatExport::Pattern4Wav ? 4 : 1;
    o.tailSeconds = what == BeatExport::PatternSampler ? 0.0 : 2.0; // a sampler loop: exactly the pattern
    o.masterBpm = es.masterBpm;
    o.sampleRate = es.sampleRate > 0.0 ? es.sampleRate : 48000.0;
    o.blockSize = es.blockSize > 0 ? es.blockSize : 256;
    {
        // The user samples it uses, looked up now (here, on the message thread); the render reads them (P4-13).
        auto samples = std::make_shared<std::map<std::string, const engine::SampleData*>>();
        for (const auto& pat : beatProject.patterns)
            for (const auto& d : pat.drums)
                if (d.sound.kind == beatmaker::SoundRef::Kind::Sample)
                    (*samples)[d.sound.samplePath] = sampleBank.get(d.sound.samplePath).data;
        o.sample = [samples](const std::string& path) -> const engine::SampleData*
        {
            const auto it = samples->find(path);
            return it != samples->end() ? it->second : nullptr;
        };
    }
    auto project = std::make_shared<const beatmaker::Project>(beatProject);
    const auto letter = juce::String::charToString(static_cast<juce::juce_wchar>('A' + o.pattern));
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H%M%S");
    const auto baseName = juce::String("Beat ") + (o.song ? juce::String("song") : "pattern " + letter);
    const auto safe = juce::Component::SafePointer<MainComponent>(this);
    const auto renderTo = [project, o](const juce::File& file, std::function<void(juce::String error, double bpm)> done)
    {
        juce::Thread::launch(
            [project, o, file, done]
            {
                const auto r = app::renderBeat(*project, o);
                file.getParentDirectory().createDirectory();
                const auto error = app::writeLoopWav(*r.audio, 0, r.audio->numSamples(), file, 24);
                const double bpm = r.bpm;
                juce::MessageManager::callAsync([done, error, bpm] { done(error, bpm); });
            });
    };
    const auto report = [safe](const juce::String& text)
    {
        if (safe != nullptr)
            safe->libraryBrowser->setStatus(text);
        util::logInfo("Beat Maker export: {}", text.toStdString());
    };
    switch (what)
    {
        case BeatExport::PatternWav:
        case BeatExport::Pattern4Wav:
        case BeatExport::SongWav:
        {
            beatRendersFolder().createDirectory();
            beatExportChooser = std::make_unique<juce::FileChooser>(
                "Export the beat as a WAV file", beatRendersFolder().getChildFile(juce::File::createLegalFileName(baseName + ".wav")),
                "*.wav");
            beatExportChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                                           [renderTo, report](const juce::FileChooser& fc)
                                           {
                                               auto file = fc.getResult();
                                               if (file == juce::File())
                                                   return;
                                               if (!file.hasFileExtension("wav"))
                                                   file = file.withFileExtension("wav");
                                               renderTo(file, [file, report](juce::String error, double)
                                                        { report(error.isEmpty() ? "saved " + file.getFullPathName() : error); });
                                           });
            break;
        }
        case BeatExport::PatternLibrary:
        case BeatExport::SongLibrary:
        {
            const auto file = beatRendersFolder().getChildFile(juce::File::createLegalFileName(baseName + " " + stamp + ".wav"));
            renderTo(file, [safe, file, report](juce::String error, double)
                     {
                         if (error.isNotEmpty())
                             report(error);
                         else if (safe != nullptr)
                             safe->importToLibrary({file}); // added (and analyzed) like any other track
                     });
            break;
        }
        case BeatExport::PatternSampler:
            juce::Thread::launch(
                [safe, project, o, letter]
                {
                    std::shared_ptr<const engine::TrackBuffer> loop = app::renderBeat(*project, o).audio;
                    const double bpm = project->followMaster ? o.masterBpm : project->ownBpm;
                    juce::MessageManager::callAsync(
                        [safe, loop, bpm, letter]
                        {
                            if (safe == nullptr)
                                return;
                            auto& self = *safe;
                            // The shown bank's first empty slot, else any empty one, else the bank's first.
                            const auto& st = self.engine.readState();
                            const int bank = self.samplerPanel.bank() * engine::Sampler::kSlotsPerBank;
                            int slot = -1;
                            for (int i = 0; i < engine::Sampler::kSlots && slot < 0; ++i)
                                if (const int c = (bank + i) % engine::Sampler::kSlots; !st.sampler[static_cast<std::size_t>(c)].loaded)
                                    slot = c;
                            if (slot < 0)
                                slot = bank;
                            const auto* raw = loop.get();
                            const auto id = self.registry.add(loop);
                            if (!self.engine.send(engine::SamplerLoad{static_cast<std::uint8_t>(slot), raw, id, static_cast<float>(bpm)}))
                            {
                                self.registry.release(id);
                                return;
                            }
                            self.samplerPanel.setSlotName(slot, "Beat " + letter + " " + juce::String(bpm, 1));
                            self.libraryBrowser->setStatus("Pattern " + letter + " is in sampler slot " + juce::String(slot + 1));
                        });
                });
            break;
    }
}

void MainComponent::setBeatSample(int track, const juce::File& file)
{
    const auto lookup = sampleBank.get(file.getFullPathName().toStdString());
    if (lookup.data == nullptr)
    {
        libraryBrowser->setStatus("Beat Maker: " + lookup.error);
        return;
    }
    const int pattern = beatPanel.grid().pattern();
    beatPanel.edit(
        [&](beatmaker::Project& p)
        {
            auto& drums = p.patterns[static_cast<std::size_t>(pattern)].drums;
            if (track >= 0 && track < static_cast<int>(drums.size()))
                beatmaker::setSample(drums[static_cast<std::size_t>(track)], file.getFullPathName().toStdString(),
                                     file.getFileNameWithoutExtension().toStdString());
        });
    beatPanel.grid().modelChanged();
}

void MainComponent::chooseBeatSample(int track)
{
    beatExportChooser = std::make_unique<juce::FileChooser>("A sample for drum track " + juce::String(track + 1),
                                                            samplesFolder(), "*.wav;*.aif;*.aiff;*.flac");
    beatExportChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                   [this, track](const juce::FileChooser& fc)
                                   {
                                       if (const auto file = fc.getResult(); file.existsAsFile())
                                           setBeatSample(track, file);
                                   });
}

void MainComponent::stopBeatMaker()
{
    beatProducer->stop();
    engine.send(engine::BeatMakerStop{}); // what is already queued goes too; what sounds fades out
    beatPlaying = false;
}

void MainComponent::beatProjectChanged()
{
    // The producer reads its own copy (on its thread).
    beatProducer->setProject(std::make_shared<const beatmaker::Project>(beatProject));
    beatStrip.setTempo(beatProject.followMaster, beatProject.ownBpm);
    // The voices' sounds, pattern x 16 + track: only those that changed.
    for (int p = 0; p < beatmaker::kPatternCount; ++p)
    {
        const auto& pat = beatProject.patterns[static_cast<std::size_t>(p)];
        for (std::size_t t = 0; t < pat.drums.size() && t < 16; ++t)
        {
            const auto index = static_cast<std::size_t>(p * 16) + t;
            const auto& sound = pat.drums[t].sound;
            if (sound.kind == beatmaker::SoundRef::Kind::Sample) // a user sample (P4-13); missing: nothing plays
            {
                const auto* data = sampleBank.get(sound.samplePath).data;
                if (sentSamples[index] != data && engine.send(engine::SetSampleSound{static_cast<std::uint16_t>(index), data}))
                    sentSamples[index] = data;
                sentDrumSounds[index].reset();
                continue;
            }
            if (sentDrumSounds[index] != sound && engine.send(engine::SetDrumSound{static_cast<std::uint16_t>(index),
                                                                                   beatmaker::engineDrumSound(sound)}))
                sentDrumSounds[index] = sound;
        }
        for (std::size_t t = 0; t < pat.synths.size() && t < 16; ++t)
        {
            const auto index = static_cast<std::size_t>(p * 16) + t;
            const auto& preset = pat.synths[t].preset;
            if (sentSynthSounds[index] != preset && engine.send(engine::SetSynthSound{static_cast<std::uint16_t>(index),
                                                                                      beatmaker::synthParamsOf(preset)}))
                sentSynthSounds[index] = preset;
        }
    }
}

juce::File MainComponent::beatProjectFile()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Devil-DJ")
        .getChildFile("Beats")
        .getChildFile(juce::String("Current") + beatmaker::kFileExtension);
}

void MainComponent::switchBottomPanel()
{
    // Browser -> Beat Maker -> transition editor -> (Code Beat, Phase 5) -> browser.
    bottomPanel = (bottomPanel + 1) % 3;
    resized();
    if (bottomPanel == 1)
        beatPanel.showView(beatPanel.view());
    else if (bottomPanel == 2)
    {
        refreshMix(false);
        mixEditor.grabKeyboardFocus();
    }
    else
        grabKeyboardFocus();
}

void MainComponent::loadMixPresetNames()
{
    juce::StringArray builtIn, user;
    for (int i = 0; i < app::kTransitionPresetCount; ++i)
        builtIn.add(app::presetName(static_cast<app::TransitionPreset>(i)));
    for (const auto& u : app::loadUserPresets())
        user.add(u.name);
    mixEditor.setPresetNames(builtIn, user);
}

void MainComponent::refreshMix(bool replan)
{
    // The next transition: from the deck leading (else playing) to the other, both loaded and analyzed.
    const auto& s = engine.readState();
    int from = s.masterSource >= 0 && s.masterSource < 2 ? s.masterSource : -1;
    if (from < 0)
        from = s.decks[0].playing ? 0 : s.decks[1].playing ? 1 : 0;
    const int to = 1 - from;
    const auto outTrack = registry.get(s.decks[from].bufferId), inTrack = registry.get(s.decks[to].bufferId);
    if (outTrack == nullptr || inTrack == nullptr)
    {
        mixPlan.reset();
        mixOutTrack = mixInTrack = nullptr;
        mixEditor.setTracks({}, {});
        mixEditor.setMessage("Load a track on each deck (one playing): the transition between them is edited here.");
        return;
    }
    const auto title = [](const engine::TrackBuffer& t) { return t.title.isNotEmpty() ? t.title : t.file.getFileNameWithoutExtension(); };
    mixOutTrack = outTrack;
    mixInTrack = inTrack;
    const auto& og = deckGrids[static_cast<std::size_t>(from)];
    const auto& ig = deckGrids[static_cast<std::size_t>(to)];
    mixEditor.setTracks({&outTrack->waveform, og.grid, outTrack->numSamples(), title(*outTrack)},
                        {&inTrack->waveform, ig.grid, inTrack->numSamples(), title(*inTrack)});
    const bool same = mixPlan && mixPlan->from == from && mixPlan->fromBuffer == s.decks[from].bufferId &&
                      mixPlan->toBuffer == s.decks[to].bufferId;
    if (same && !replan)
        return;
    MixPlan m;
    m.from = from;
    m.fromBuffer = s.decks[from].bufferId;
    m.toBuffer = s.decks[to].bufferId;
    m.fromTrack = og.trackId;
    m.toTrack = ig.trackId;
    if (const auto planned = replan ? std::nullopt : transitionFor(from))
        m.edit = *planned;
    else if (replan)
    {
        if (const auto autoPlan = transitionFor(from)) // the saved one was deleted: this is Auto
            m.edit = *autoPlan;
    }
    if (m.edit.curves.from[0].points.empty() && m.edit.curves.to[0].points.empty())
    {
        // Not analyzed enough for Auto: a smooth blend from the next phrase.
        m.edit.curves = app::makePreset(app::TransitionPreset::SmoothBlend, 32.0f);
        m.edit.outBeat = std::ceil(s.decks[from].beat / 16.0) * 16.0;
        m.edit.inBeat = app::firstDownbeatBeat(ig.grid);
        m.edit.preset = "Smooth blend";
        m.edit.reasons = {"the tracks aren't analyzed enough for Auto: a smooth blend from the next phrase"};
    }
    mixPlan = m;
    mixEditor.setEdit(m.edit);
}

std::optional<ui::TransitionEditor::Edit> MainComponent::transitionFor(int from, bool* saved)
{
    // The pair on the decks: its saved transition (P4-19), else the Auto plan; nothing when not analyzed enough.
    const auto& s = engine.readState();
    const int to = 1 - from;
    if (saved != nullptr)
        *saved = false;
    const auto& og = deckGrids[static_cast<std::size_t>(from)];
    const auto& ig = deckGrids[static_cast<std::size_t>(to)];
    if (gridReader != nullptr && og.trackId != 0 && ig.trackId != 0)
        if (const auto t = app::loadTransition(*gridReader, og.trackId, ig.trackId))
        {
            if (saved != nullptr)
                *saved = true;
            ui::TransitionEditor::Edit e;
            e.curves = t->curves;
            e.outBeat = t->outBeat;
            e.inBeat = t->inBeat;
            e.preset = t->preset;
            e.reasons = {"your saved transition for these two tracks (AUTO plans it again)"};
            return e;
        }
    const auto outTrack = registry.get(s.decks[from].bufferId), inTrack = registry.get(s.decks[to].bufferId);
    if (outTrack == nullptr || inTrack == nullptr)
        return std::nullopt;
    app::AutoInput a{&outTrack->waveform, og.grid, outTrack->numSamples(), og.musicalKey, og.grid.firstBpm()};
    app::AutoInput b{&inTrack->waveform, ig.grid, inTrack->numSamples(), ig.musicalKey, ig.grid.firstBpm()};
    const auto plan = app::planAutoTransition(a, b);
    if (!plan.ok)
        return std::nullopt;
    ui::TransitionEditor::Edit e;
    e.curves = plan.curves;
    e.outBeat = plan.outBeat;
    e.inBeat = plan.inBeat;
    e.preset = juce::String("Auto: ") + app::presetName(plan.preset);
    e.reasons = plan.reasons;
    return e;
}

void MainComponent::setSpotifyApp(const juce::String& clientId, int redirectPort)
{
    spotifyClientId = clientId.trim();
    spotifyPort = std::clamp(redirectPort, 1024, 65535);
    app::SpotifyAuth::Config c;
    c.clientId = spotifyClientId;
    c.redirectPort = spotifyPort;
    c.tokenFile = cacheFolder().getParentDirectory().getChildFile("spotify.tok"); // sealed for this Windows user
    spotify = std::make_unique<app::SpotifyAuth>(c);
    if (onSpotifyStateChanged)
        onSpotifyStateChanged();
}

void MainComponent::spotifyMenu(int op)
{
    if (op == 0)
    {
        // The Client ID of the owner's own Spotify app (developer.spotify.com > Dashboard > Create app; its Redirect
        // URI must be exactly the one shown here).
        auto* w = new juce::AlertWindow(
            "Spotify app",
            "Create a free app at developer.spotify.com (Dashboard > Create app), add this Redirect URI to it:\n\n"
            "    http://127.0.0.1:" + juce::String(spotifyPort) + "/callback\n\n"
            "tick \"Web API\", save, and paste its Client ID here. Devil-DJ only reads your playlists' track lists "
            "and matches them to your own files; it never plays Spotify audio.",
            juce::MessageBoxIconType::NoIcon);
        w->addTextEditor("id", spotifyClientId, "Client ID");
        w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        w->enterModalState(true, juce::ModalCallbackFunction::create(
                                     [this, w](int result)
                                     {
                                         if (result != 1)
                                             return;
                                         const auto id = w->getTextEditorContents("id").trim();
                                         setSpotifyApp(id, spotifyPort);
                                         if (onSpotifyClientIdChanged)
                                             onSpotifyClientIdChanged(id);
                                     }),
                           true);
        return;
    }
    if (spotify == nullptr)
        setSpotifyApp(spotifyClientId, spotifyPort);
    if (op == 3)
    {
        showSpotifyImport();
        return;
    }
    if (op == 2)
    {
        spotify->signOut();
        libraryBrowser->setStatus("Signed out of Spotify.");
        if (onSpotifyStateChanged)
            onSpotifyStateChanged();
        return;
    }
    libraryBrowser->setStatus("Signing in to Spotify in your browser...");
    spotify->beginSignIn([](const juce::URL& url) { url.launchInDefaultBrowser(); },
                         [safe = juce::Component::SafePointer<MainComponent>(this)](bool ok, juce::String message)
                         {
                             juce::MessageManager::callAsync(
                                 [safe, ok, message]
                                 {
                                     if (safe == nullptr)
                                         return;
                                     safe->libraryBrowser->setStatus(message);
                                     util::logInfo("Spotify: {}", ok ? "signed in" : message.toStdString());
                                     if (safe->onSpotifyStateChanged)
                                         safe->onSpotifyStateChanged();
                                 });
                         });
}

void MainComponent::showSpotifyImport()
{
    if (spotify == nullptr || !spotify->signedIn() || libraryDb == nullptr)
    {
        libraryBrowser->setStatus("Sign in to Spotify first (Decks > Spotify > Sign in).");
        return;
    }
    if (spotifyImportWindow != nullptr)
    {
        spotifyImportWindow->toFront(true);
        return;
    }
    // The API reads with the sign-in's token (refreshed when needed); the library through its own reader.
    auto* auth = spotify.get();
    auto api = std::make_shared<app::SpotifyApi>([auth](bool force, juce::String* error)
                                                 { return force ? auth->refreshNow(error) : auth->accessToken(error); });
    auto* db = libraryDb;
    app::SpotifyImportWindow::Source source;
    source.playlists = [api] { return api->playlists(); };
    source.tracks = [api](const std::string& id) { return api->tracks(id); };
    source.library = [db]
    {
        auto reader = db->openReader();
        return reader ? app::loadLibraryTracks(*reader.value()) : std::vector<app::LibraryTrack>{};
    };
    source.create = [db](const app::ImportPlan& plan)
    {
        util::Result<std::int64_t> result = util::Result<std::int64_t>::fail("not written");
        db->writeAndWait([&](library::Connection& c) { result = app::createImportedPlaylist(c, plan); });
        return result;
    };
    source.saveText = [this](const juce::String& text)
    {
        spotifyChooser = std::make_unique<juce::FileChooser>(
            "Save the tracks not in your library",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Spotify tracks not in my library.txt"), "*.txt");
        spotifyChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                                    [text](const juce::FileChooser& fc)
                                    {
                                        if (const auto f = fc.getResult(); f != juce::File())
                                            f.replaceWithText(text);
                                    });
    };
    auto* content = new app::SpotifyImportWindow(source);
    content->onDone = [this](int created)
    {
        libraryBrowser->setStatus(juce::String(created) + " playlists imported from Spotify (matched to your own files).");
    };
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(content);
    o.dialogTitle = "Import Spotify playlists";
    o.dialogBackgroundColour = juce::Colour(0xff16161b);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    spotifyImportWindow = o.launchAsync();
    content->start();
}

void MainComponent::saveMixPlan()
{
    if (!mixPlan || libraryDb == nullptr || mixPlan->fromTrack == 0 || mixPlan->toTrack == 0)
        return;
    app::SavedTransition t;
    t.fromTrack = mixPlan->fromTrack;
    t.toTrack = mixPlan->toTrack;
    t.preset = mixPlan->edit.preset.startsWith("Auto: ") ? mixPlan->edit.preset.fromFirstOccurrenceOf("Auto: ", false, false)
                                                        : mixPlan->edit.preset;
    t.curves = mixPlan->edit.curves;
    t.outBeat = mixPlan->edit.outBeat;
    t.inBeat = mixPlan->edit.inBeat;
    libraryDb->write([t](library::Connection& c) { app::saveTransition(c, t); });
    transitionLabels.set(t.fromTrack, t.toTrack, t.preset);
    libraryBrowser->repaintRows();
}

void MainComponent::startPlannedTransition(const ui::TransitionEditor::Edit& edit, int from, bool isPreview)
{
    const auto& s = engine.readState();
    const auto f = static_cast<engine::DeckId>(from), t = static_cast<engine::DeckId>(1 - from);
    const auto& og = deckGrids[f].grid;
    const auto& ig = deckGrids[t].grid;
    if (og.empty() || ig.empty())
        return;
    // The incoming deck cued where it comes in from.
    const double inSample = ig.sampleAt(edit.inBeat);
    engine.send(engine::SetCuePoint{t, inSample});
    engine.send(engine::Seek{t, inSample});
    engine.send(engine::SetSync{t, true});
    auto curves = std::make_shared<const engine::TransitionCurves>(edit.curves);
    const auto begin = [this, f, t, curves, outBeat = edit.outBeat]
    {
        // The outgoing track's beat in master-clock beats (the clock follows the leading deck plus whole bars).
        const auto& st = engine.readState();
        const double toMaster = st.masterBeat - st.decks[f].beat;
        double start = outBeat + toMaster;
        if (start < st.masterBeat + 0.5) // already past it: the next phrase line
            start = std::ceil((st.masterBeat + 0.5) / 16.0) * 16.0;
        engine.startTransition(engine::StartTransition{f, t, 0.0f, engine::TransitionStyle::Blend, start}, curves);
    };
    if (!isPreview)
    {
        begin();
        return;
    }
    // Preview: the outgoing track from 2 bars before, then the transition on its beat (once the seek is in).
    engine.send(engine::CancelTransition{});
    engine.send(engine::Pause{t});
    engine.send(engine::Seek{f, og.sampleAt(edit.outBeat - 8.0)});
    engine.send(engine::Play{f});
    juce::Timer::callAfterDelay(150, [safe = juce::Component::SafePointer<MainComponent>(this), begin]
                                {
                                    if (safe != nullptr)
                                        begin();
                                });
    (void)s;
}

void MainComponent::showSuggestions()
{
    auto s = nextTrackPanel.state();
    s.intent = matcherIntent;
    s.keyPolicy = matcherKeyPolicy;
    s.scope = matcherScope;
    s.preset = matcherPreset;
    s.highlight = suggestionHighlight;
    s.energyHistory = energyHistory;
    s.rows.clear();
    s.keyA = -1;
    s.context = {};
    if (gridReader != nullptr)
    {
        library::TrackRepository tracks(*gridReader);
        if (const auto a = tracks.get(suggestionsFor))
            s.keyA = a->musicalKey.value_or(-1);
        for (const auto& sug : suggestions)
        {
            ui::NextTrackPanel::Row row;
            if (const auto t = tracks.get(sug.trackId))
            {
                row.title = juce::String::fromUTF8(t->title.c_str());
                if (row.title.isEmpty())
                    row.title = juce::File(juce::String::fromUTF8(t->path.c_str())).getFileNameWithoutExtension();
                row.artist = juce::String::fromUTF8(t->artist.c_str());
                row.bpm = t->bpm.value_or(0.0);
                row.key = t->musicalKey.value_or(-1);
            }
            row.energy = energyOf(sug.trackId);
            row.score = sug.score;
            row.wildcard = sug.wildcard;
            for (const auto& [text, value] : sug.chips)
                row.chips.push_back({juce::String::fromUTF8(text.c_str()), ui::NextTrackPanel::chipLevel(value)});
            row.breakdown = juce::String::fromUTF8(sug.breakdown.c_str());
            s.rows.push_back(std::move(row));
        }
    }
    if (s.rows.empty())
        s.context = matcherService == nullptr ? "No library: suggestions need it."
                                              : "Play a track from the library: suggestions for the other deck appear here.";
    nextTrackPanel.setState(std::move(s));
}

void MainComponent::previewSuggestion(bool down)
{
    // Ctrl+N held: the highlighted suggestion on the idle deck, in the headphones only (its fader down meanwhile),
    // from a third of the way in; let go: paused, everything back.
    const auto& s = engine.readState();
    if (!down)
    {
        if (preview.on && preview.deck >= 0)
        {
            const auto d = static_cast<engine::DeckId>(preview.deck);
            engine.send(engine::Pause{d});
            engine.send(engine::Seek{d, s.decks[d].cuePointSamples});
            engine.send(engine::SetParam{engine::channelParam(d, engine::ChannelParam::Fader), preview.fader});
            engine.send(engine::SetParam{engine::channelParam(d, engine::ChannelParam::Cue), preview.cue ? 1.0f : 0.0f});
        }
        preview.on = false;
        return;
    }
    const int idle = !s.decks[0].playing ? 0 : !s.decks[1].playing ? 1 : -1;
    if (idle < 0 || suggestions.empty())
        return;
    const auto& pick = suggestions[static_cast<std::size_t>(std::clamp(suggestionHighlight, 0, static_cast<int>(suggestions.size()) - 1))];
    if (deckTrackId(static_cast<engine::DeckId>(idle)) != pick.trackId)
    {
        loadSuggestion(); // on the deck first; the next Ctrl+N previews it
        return;
    }
    const auto d = static_cast<engine::DeckId>(idle);
    preview = {true, idle, s.channels[d].fader, s.channels[d].cue};
    engine.send(engine::SetParam{engine::channelParam(d, engine::ChannelParam::Fader), 0.0f});
    engine.send(engine::SetParam{engine::channelParam(d, engine::ChannelParam::Cue), 1.0f});
    engine.send(engine::Seek{d, s.decks[d].lengthSamples / 3.0});
    engine.send(engine::Play{d});
}

void MainComponent::setNextTrackPanel(bool show)
{
    showNextTrack = show;
    resized();
    if (onNextTrackPanelChanged)
        onNextTrackPanelChanged(show);
}

void MainComponent::loadSuggestion()
{
    const auto& s = engine.readState();
    if (suggestions.empty() || gridReader == nullptr)
        return;
    // The idle deck: the one not playing (if both are, nothing).
    const int idle = !s.decks[0].playing ? 0 : !s.decks[1].playing ? 1 : -1;
    if (idle < 0)
        return;
    const auto& pick = suggestions[static_cast<std::size_t>(std::clamp(suggestionHighlight, 0, static_cast<int>(suggestions.size()) - 1))];
    if (const auto t = library::TrackRepository(*gridReader).get(pick.trackId))
        loadFile(static_cast<engine::DeckId>(idle), juce::File(juce::String::fromUTF8(t->path.c_str())));
}

void MainComponent::applyAutomixSettings()
{
    const auto& p = automixPanel.settings();
    app::Automix::Settings s;
    s.repeat = p.repeat;
    s.random = p.random;
    s.transitionBeats = p.beats;
    s.smart = p.smart;
    s.style = p.style == 1 ? engine::TransitionStyle::FilterSweep
              : p.style == 2 ? engine::TransitionStyle::EchoOut
                             : engine::TransitionStyle::Blend;
    automix.setSettings(s);
}

void MainComponent::queueAutomix()
{
    automixFiles.clear();
    automixNames.clear();
    automixIds.clear();
    if (gridReader != nullptr)
    {
        library::TrackRepository tracks(*gridReader);
        for (const auto id : libraryBrowser->selectedTrackIds())
            if (const auto t = tracks.get(id))
            {
                automixFiles.push_back(juce::File(juce::String::fromUTF8(t->path.c_str())));
                automixIds.push_back(id);
                automixNames.push_back(t->title.empty() ? automixFiles.back().getFileNameWithoutExtension()
                                                        : juce::String::fromUTF8(t->title.c_str()));
            }
    }
    applyAutomixSettings();
    automix.setQueue(static_cast<int>(automixFiles.size()));
    showPanel(3);
}

void MainComponent::toggleAutomix()
{
    if (automix.running())
    {
        automix.stop(); // what plays plays on; no more transitions
        engine.send(engine::CancelTransition{});
        return;
    }
    if (automixFiles.empty())
        queueAutomix();
    if (automixFiles.empty())
        return;
    // Both decks free for it: they stop, the crossfader goes to deck A's side.
    engine.send(engine::Pause{0});
    engine.send(engine::Pause{1});
    automixLogged = 0;
    automix.start();
    showPanel(3);
}

int MainComponent::pickAutomixNext(int current, const std::vector<int>& remaining)
{
    // Smart Automix (P3-25): the matcher's score (the Next Track panel's intent, preset and key policy) for each
    // remaining queue item against the one playing; those passing the matcher's filters first.
    const auto idOf = [this](int i)
    { return i >= 0 && i < static_cast<int>(automixIds.size()) ? automixIds[static_cast<std::size_t>(i)] : std::int64_t{0}; };
    if (gridReader == nullptr || remaining.empty())
        return -1;
    const auto now = static_cast<std::int64_t>(juce::Time::currentTimeMillis() / 1000);
    const auto a = matcher::loadTrack(*gridReader, idOf(current), now);
    if (!a)
        return -1;
    matcher::Context c;
    c.a = *a;
    c.aBpmNow = a->bpm;
    c.intent = matcherIntent == 0 ? matcher::Intent::Down : matcherIntent == 2 ? matcher::Intent::Up
              : matcherIntent == 3                       ? matcher::Intent::Switch
                                                         : matcher::Intent::Hold;
    c.keyPolicy = matcherKeyPolicy == 0 ? matcher::KeyPolicy::Strict : matcherKeyPolicy == 2 ? matcher::KeyPolicy::Off
                                                                                           : matcher::KeyPolicy::Relaxed;
    matcher::loadHistory(*gridReader, c, now);
    std::vector<matcher::TrackInfo> tracks;
    std::vector<int> items;
    for (const int i : remaining)
        if (auto t = matcher::loadTrack(*gridReader, idOf(i), now))
        {
            tracks.push_back(std::move(*t));
            items.push_back(i);
        }
    if (tracks.empty())
        return -1;
    const auto [c10, c90] = matcher::soundPercentiles(tracks);
    c.c10 = c10;
    c.c90 = c90;
    const auto& weights =
        matcherPresets[static_cast<std::size_t>(std::clamp(matcherPreset, 0, static_cast<int>(matcherPresets.size()) - 1))].weights;
    int best = -1;
    double bestScore = -1e9;
    for (std::size_t k = 0; k < tracks.size(); ++k)
    {
        const double s = matcher::score(c, tracks[k], weights).score + (matcher::passesFilters(c, tracks[k]) ? 10.0 : 0.0);
        if (s > bestScore)
        {
            bestScore = s;
            best = items[k];
        }
    }
    return best;
}

juce::File MainComponent::recordingsFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Devil-DJ/Recordings");
}

void MainComponent::toggleRecording()
{
    if (recorder.isRunning())
    {
        recorder.stop(); // writes what is left, closes the file (its sheets follow on the message thread)
        return;
    }
    const auto& s = recorderPanel.settings();
    engine::Recorder::Options options;
    options.folder = recordingsFolder();
    options.format = s.flac ? engine::Recorder::Format::Flac24 : engine::Recorder::Format::Wav24;
    options.sampleRate = engine.readState().sampleRate > 0.0 ? engine.readState().sampleRate : 48000.0;
    options.autoStart = s.autoStart;
    options.autoSplit = s.autoSplit;
    engine.recordTap().setSource(s.headphones ? engine::RecordTap::Source::Headphones : engine::RecordTap::Source::Master);
    if (!recorder.start(options))
        util::logWarning("Recording: could not start in {}", options.folder.getFullPathName().toStdString());
    else
        util::logInfo("Recording started ({})", s.flac ? "FLAC" : "WAV");
    showPanel(2);
}

void MainComponent::writeRecordingSheets(const engine::Recorder::FileInfo& f)
{
    util::logInfo("Recording: {} ({} s)", f.file.getFullPathName().toStdString(),
                  static_cast<double>(f.frames) / std::max(1.0, engine.readState().sampleRate));
    if (gridReader == nullptr)
        return;
    std::vector<app::PlayedAt> plays;
    library::HistoryRepository history(*gridReader);
    library::TrackRepository tracks(*gridReader);
    for (const auto& e : history.entries(historySession->load()))
    {
        app::PlayedAt p;
        p.playedAt = e.playedAt;
        if (const auto t = tracks.get(e.trackId))
        {
            p.artist = t->artist;
            p.title = t->title.empty() ? juce::File(juce::String::fromUTF8(t->path.c_str())).getFileNameWithoutExtension().toStdString()
                                       : t->title;
        }
        plays.push_back(std::move(p));
    }
    const double start = static_cast<double>(f.started.toMilliseconds()) / 1000.0;
    const double seconds = static_cast<double>(f.frames) / std::max(1.0, engine.readState().sampleRate);
    const auto list = app::tracksInRecording(plays, start, seconds);
    const auto base = f.file.withFileExtension("");
    base.withFileExtension(".txt").replaceWithText(juce::String::fromUTF8(app::tracklistText(list).c_str()));
    base.withFileExtension(".cue").replaceWithText(juce::String::fromUTF8(
        app::cueSheet(f.file.getFileName().toStdString(), f.file.hasFileExtension(".flac"), list).c_str()));
}

void MainComponent::chooseSampleFile(int slot)
{
    sampleChooser = std::make_unique<juce::FileChooser>("Load a sample", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.m4a");
    sampleChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                               [this, slot](const juce::FileChooser& chooser)
                               {
                                   const auto file = chooser.getResult();
                                   if (file.existsAsFile())
                                       loadSample(slot, file, 0.0, file.getFileNameWithoutExtension());
                               });
}

void MainComponent::loadSample(int slot, const juce::File& file, double bpm, const juce::String& name)
{
    const double rate = engine.readState().sampleRate > 0.0 ? engine.readState().sampleRate : 48000.0;
    const double master = engine.readState().masterBpm;
    juce::Thread::launch(
        [safe = juce::Component::SafePointer<MainComponent>(this), slot, file, bpm, name, rate, master]
        {
            engine::DecodeOptions options;
            options.targetSampleRate = rate;
            auto decoded = engine::decodeTrack(file, options);
            juce::MessageManager::callAsync(
                [safe, slot, bpm, name, master, result = std::make_shared<decltype(decoded)>(std::move(decoded))]
                {
                    if (safe == nullptr)
                        return;
                    auto& self = *safe;
                    if (!result->isOk())
                    {
                        util::logWarning("Sampler: a sample did not load: {}", result->error());
                        return;
                    }
                    std::shared_ptr<const engine::TrackBuffer> buffer = std::move(*result).value();
                    const double seconds = buffer->numSamples() / std::max(1.0, buffer->sampleRate);
                    const double tempo = bpm > 0.0 ? bpm : engine::Sampler::estimateLoopBpm(seconds, master);
                    const auto* raw = buffer.get();
                    const auto id = self.registry.add(std::move(buffer));
                    if (!self.engine.send(engine::SamplerLoad{static_cast<std::uint8_t>(slot), raw, id,
                                                              static_cast<float>(tempo)}))
                    {
                        self.registry.release(id);
                        return;
                    }
                    self.samplerPanel.setSlotName(slot, name);
                });
        });
}

void MainComponent::setPadFxPresets(const juce::String& json)
{
    padFxPresets = json.isEmpty() ? app::defaultPadFxPresets() : app::presetsFromJson(json);
    installPadFxBank();
}

void MainComponent::installPadFxBank()
{
    const auto& bank = padFxPresets[static_cast<std::size_t>(padFxBank)];
    std::array<juce::String, 8> names;
    std::array<std::uint32_t, 8> colours{};
    std::array<bool, 8> release{};
    for (int pad = 0; pad < app::kPadFxPads; ++pad)
    {
        const auto& p = bank[static_cast<std::size_t>(pad)];
        names[static_cast<std::size_t>(pad)] = app::presetName(p);
        colours[static_cast<std::size_t>(pad)] = app::presetColour(p);
        release[static_cast<std::size_t>(pad)] = p.release;
        for (engine::DeckId d = 0; d < 2; ++d)
        {
            std::unique_ptr<dsp::fx::Effect> fx;
            if (!p.release)
            {
                fx = dsp::fx::makeEffect(static_cast<dsp::fx::EffectType>(p.type));
                fx->setBeats(p.beats);
                for (int k = 0; k < std::min(3, fx->numParams()); ++k)
                    fx->setParam(k, p.params[static_cast<std::size_t>(k)]);
            }
            engine.installPadEffect(d, pad, std::move(fx)); // (prepares it for the device)
            engine.send(engine::PadFxCommand{d, static_cast<std::uint8_t>(pad), engine::FxOp::Wet, 0, p.wet});
        }
    }
    deckA.setPadFxLooks(names, colours, release, padFxBank);
    deckB.setPadFxLooks(names, colours, release, padFxBank);
}

void MainComponent::editPadFx(int pad, const app::PadFxPreset& preset)
{
    padFxPresets[static_cast<std::size_t>(padFxBank)][static_cast<std::size_t>(pad)] = preset;
    installPadFxBank();
    if (onPadFxPresetsChanged)
        onPadFxPresetsChanged(app::toJson(padFxPresets));
}

void MainComponent::showPadFxMenu(engine::DeckId deck, int pad)
{
    if (deck >= 2 || pad < 0 || pad >= app::kPadFxPads)
        return;
    const auto current = padFxPresets[static_cast<std::size_t>(padFxBank)][static_cast<std::size_t>(pad)];
    juce::PopupMenu m;
    m.addSectionHeader("Pad FX " + juce::String(padFxBank + 1) + "-" + juce::String(pad + 1) + ": " +
                       app::presetName(current));
    juce::PopupMenu beatFx, releaseFx;
    for (int t = 0; t < dsp::fx::kEffectTypeCount; ++t)
        beatFx.addItem(dsp::fx::effectTypeName(static_cast<dsp::fx::EffectType>(t)), true,
                       !current.release && current.type == t,
                       [this, pad, t, current]
                       {
                           auto p = app::presetFor(false, t);
                           p.beats = current.beats;
                           p.latch = current.latch;
                           editPadFx(pad, p);
                       });
    for (int k = 0; k < dsp::fx::ReleaseFx::kKindCount; ++k)
        releaseFx.addItem(dsp::fx::ReleaseFx::kindName(static_cast<dsp::fx::ReleaseFx::Kind>(k)), true,
                          current.release && current.type == k,
                          [this, pad, k, current]
                          {
                              auto p = app::presetFor(true, k);
                              p.beats = current.beats;
                              p.wet = 1.0f;
                              editPadFx(pad, p);
                          });
    m.addSubMenu("Effect", beatFx);
    m.addSubMenu("Release effect", releaseFx);
    juce::PopupMenu beatsMenu;
    for (const double b : ui::FxPanel::kBeatSteps)
        beatsMenu.addItem(ui::FxPanel::beatsText(b), true, std::abs(current.beats - b) < 1e-6,
                          [this, pad, current, b]
                          {
                              auto p = current;
                              p.beats = static_cast<float>(b);
                              editPadFx(pad, p);
                          });
    m.addSubMenu("Beats", beatsMenu);
    if (!current.release)
    {
        juce::PopupMenu wetMenu;
        for (const float w : {0.25f, 0.5f, 0.6f, 0.75f, 1.0f})
            wetMenu.addItem(juce::String(juce::roundToInt(w * 100.0f)) + " %", true, std::abs(current.wet - w) < 1e-4f,
                            [this, pad, current, w]
                            {
                                auto p = current;
                                p.wet = w;
                                editPadFx(pad, p);
                            });
        m.addSubMenu("Wet", wetMenu);
        // The knobs of the effect, in quarters.
        if (auto fx = dsp::fx::makeEffect(static_cast<dsp::fx::EffectType>(current.type)))
            for (int k = 0; k < std::min(3, fx->numParams()); ++k)
            {
                juce::PopupMenu knob;
                for (const float v : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
                    knob.addItem(juce::String(juce::roundToInt(v * 100.0f)) + " %", true,
                                 std::abs(current.params[static_cast<std::size_t>(k)] - v) < 1e-4f,
                                 [this, pad, current, k, v]
                                 {
                                     auto p = current;
                                     p.params[static_cast<std::size_t>(k)] = v;
                                     editPadFx(pad, p);
                                 });
                m.addSubMenu(fx->param(k).name, knob);
            }
        m.addItem("Latched (press on, press off)", true, current.latch,
                  [this, pad, current]
                  {
                      auto p = current;
                      p.latch = !p.latch;
                      editPadFx(pad, p);
                  });
    }
    // A snapshot of an FX unit's first slot: its effect, beats, wet and knobs as they are now.
    for (int u = 0; u < 2; ++u)
    {
        const int type = fxPanel.effectType(u, 0);
        m.addItem("Snapshot of FX" + juce::String(u + 1) + " slot 1", type >= 0, false,
                  [this, pad, u, type]
                  {
                      auto p = app::presetFor(false, type);
                      const auto& slot = engine.readState().fx[static_cast<std::size_t>(u)].slots[0];
                      p.beats = slot.beats;
                      p.wet = slot.wet;
                      for (int k = 0; k < 3; ++k)
                          p.params[static_cast<std::size_t>(k)] = static_cast<float>(
                              fxPanel.unit(u).slots[0].knobs[static_cast<std::size_t>(k)]->getValue());
                      editPadFx(pad, p);
                  });
    }
    m.addSeparator();
    m.addItem("Default for this pad", true, false,
              [this, pad]
              {
                  editPadFx(pad, app::defaultPadFxPresets()[static_cast<std::size_t>(padFxBank)]
                                                           [static_cast<std::size_t>(pad)]);
              });
    m.addItem(padFxBank == 0 ? "Show bank 2 (Shift + pad 2)" : "Show bank 1 (Shift + pad 1)",
              [this]
              {
                  padFxBank = 1 - padFxBank;
                  installPadFxBank();
              });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&(deck == 0 ? deckA : deckB).padButton(pad)));
}

void MainComponent::showPadModeMenu(engine::DeckId deck)
{
    if (deck >= 2)
        return;
    static constexpr const char* keys[] = {"F1",       "F2",       "F3",       "F4",      "Shift+F1",
                                           "Shift+F2", "Shift+F3", "Shift+F4", "no key",   "no key",
                                           "no key"};
    juce::PopupMenu m;
    m.addSectionHeader(juce::String("Deck ") + engine::deckLetter(deck) + " pads");
    for (int i = 0; i < ui::kPadModeCount; ++i)
    {
        const auto mode = static_cast<ui::PadMode>(i);
        juce::String name = ui::padModeName(mode);
        name << "  (" << keys[i] << ")";
        if (!ui::padModeAvailable(mode))
            name << "  - comes with " << ui::padModeComesWith(mode);
        m.addItem(name, true, padModes[deck] == mode, [this, deck, mode] { setPadMode(deck, mode); });
    }
    m.addSeparator();
    m.addItem("Capture the slicer range to the sampler", engine.readState().decks[deck].slicerOn, false,
              [this, deck] { captureToSampler(deck, true); });
    for (const double beats : {8.0, 16.0, 32.0})
        m.addItem("Slicer: " + juce::String(juce::roundToInt(beats)) + " beats", true, slicerBeats[deck] == beats,
                  [this, deck, beats]
                  {
                      slicerBeats[deck] = beats;
                      if (padModes[deck] == ui::PadMode::Slicer)
                          engine.send(engine::SetSlicer{deck, true, beats});
                      else
                          setPadMode(deck, ui::PadMode::Slicer);
                  });
    m.addItem(hotCuePage == 0 ? "Hot cues: show pads I-P (F1)" : "Hot cues: show pads A-H (F1)",
              [this, deck]
              {
                  setHotCuePage(1 - hotCuePage);
                  setPadMode(deck, ui::PadMode::HotCue);
              });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&(deck == 0 ? deckA : deckB).padModeButton()));
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
        {
            util::logInfo("Analysis: done ({} analyzed, {} failed)", s.done, s.failed);
            ++analysisRuns; // the matcher looks again (P3-19)
        }
        analysisTotal = 0;
        libraryBrowser->setActivity({});
        return;
    }
    // "Analyzing 12 of 500" - the total grows when an import adds tracks.
    analysisTotal = std::max(analysisTotal, s.done + s.failed + s.running + s.queued);
    const int finished = std::min(analysisTotal, s.done + s.failed);
    libraryBrowser->setActivity("Analyzing " + juce::String(finished + 1) + " of " + juce::String(analysisTotal));
}

void MainComponent::setLayout(int waveforms, bool bigBrowser)
{
    waveLayout = waveforms == 1 ? app::WaveLayout::Vertical : app::WaveLayout::Horizontal;
    if (bigBrowser != browserBig)
    {
        browserBig = bigBrowser;
        libraryBrowser->setBig(browserBig);
    }
    resized();
    if (onLayoutChanged)
        onLayoutChanged(waveforms == 1 ? 1 : 0, browserBig);
}

void MainComponent::toggleBrowserSize()
{
    browserBig = !browserBig;
    if (onLayoutChanged)
        onLayoutChanged(waveLayout == app::WaveLayout::Vertical ? 1 : 0, browserBig);
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
        // A full map saved by the keymap editor (P3-20), or overrides on the default map (P1-16).
        bindings = ui::keymapFromJson(file.loadFileAsString().toStdString(), errors);
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

void MainComponent::editKeymap()
{
    if (keymapEditor != nullptr)
    {
        keymapEditor->toFront(true);
        return;
    }
    auto* editor = new ui::KeymapEditor(keyboard.currentBindings());
    keymapEditor = editor;
    editor->onSave = [this](const std::vector<ui::KeyBinding>& map)
    {
        const auto folder = keymapFolder();
        folder.createDirectory();
        folder.getChildFile("keymap.json").replaceWithText(juce::String::fromUTF8(ui::keymapToJson(map).c_str()));
        reloadKeymap();
    };
    editor->onExport = [this](const std::vector<ui::KeyBinding>& map)
    {
        keymapChooser = std::make_unique<juce::FileChooser>("Export the keymap", keymapFolder().getChildFile("my keymap.json"), "*.json");
        keymapChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                                   [map](const juce::FileChooser& c)
                                   {
                                       const auto file = c.getResult();
                                       if (file != juce::File())
                                           file.replaceWithText(juce::String::fromUTF8(ui::keymapToJson(map).c_str()));
                                   });
    };
    editor->onImport = [this]
    {
        keymapChooser = std::make_unique<juce::FileChooser>("Import a keymap", keymapFolder(), "*.json");
        keymapChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                   [this](const juce::FileChooser& c)
                                   {
                                       const auto file = c.getResult();
                                       if (!file.existsAsFile() || keymapEditor == nullptr)
                                           return;
                                       std::vector<std::string> errors;
                                       keymapEditor->setBindings(ui::keymapFromJson(file.loadFileAsString().toStdString(), errors));
                                   });
    };
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(editor);
    options.dialogTitle = "Keymap";
    options.dialogBackgroundColour = juce::Colour(0xff16161b);
    options.escapeKeyTriggersCloseButton = false; // Esc may be the key being set
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.launchAsync();
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
    // P3-23: the layout (horizontal / vertical waveforms, the enlarged browser) is computed in app/MainLayout.
    app::LayoutSizes sizes;
    sizes.mixerPreferredWidth = ui::MixerView::kPreferredWidth;
    sizes.mixerMinimumHeight = ui::MixerView::kMinimumHeight;
    sizes.mixerPreferredHeight = ui::MixerView::kPreferredHeight;
    sizes.panelHeight = ui::FxPanel::kPreferredHeight;
    const auto l = app::computeLayout(getLocalBounds(), waveLayout, browserBig, sizes, showNextTrack);
    nextTrackPanel.setBounds(l.nextTrack);
    nextTrackPanel.setVisible(!l.nextTrack.isEmpty() && bottomPanel == 0);
    {
        // The Beat Maker and, at its right, its channel strip (P4-10).
        auto area = l.nextTrack.isEmpty() ? l.browser : l.browser.getUnion(l.nextTrack);
        beatStrip.setBounds(area.removeFromRight(std::min(140, area.getWidth() / 6)));
        beatPanel.setBounds(area);
        beatPanel.setVisible(bottomPanel == 1);
        beatStrip.setVisible(bottomPanel == 1);
        mixEditor.setBounds(l.nextTrack.isEmpty() ? l.browser : l.browser.getUnion(l.nextTrack));
        mixEditor.setVisible(bottomPanel == 2);
    }
    deckA.setBounds(l.deckA);
    deckB.setBounds(l.deckB);
    mixer.setBounds(l.mixer);
    for (const auto& [wave, rect] : {std::pair{&waveA, l.waveA}, std::pair{&waveB, l.waveB}})
    {
        if (l.wavesVertical)
        {
            // A column with time running down: the view keeps its horizontal drawing, turned a quarter.
            wave->setTransform({});
            wave->setBounds(0, 0, rect.getHeight(), rect.getWidth());
            wave->setTransform(app::verticalWaveTransform(rect));
        }
        else
        {
            wave->setTransform({});
            wave->setBounds(rect);
        }
        wave->setVisible(l.wavesVisible);
    }
    libraryBrowser->setBounds(l.browser);
    libraryBrowser->setVisible(bottomPanel == 0);
    juce::TextButton* tabs[] = {&fxTab, &samplerTab, &recordTab, &automixTab};
    for (std::size_t i = 0; i < 4; ++i)
    {
        tabs[i]->setBounds(l.tabs[i]);
        tabs[i]->setVisible(l.panelVisible);
    }
    juce::Component* panels[] = {&fxPanel, &samplerPanel, &recorderPanel, &automixPanel};
    for (int i = 0; i < 4; ++i)
    {
        panels[i]->setBounds(l.panel);
        panels[i]->setVisible(l.panelVisible && shownPanel == i);
    }
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
