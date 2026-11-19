#include "MainComponent.h"

#include "engine/TrackDecoder.h"
#include "library/Database.h"
#include "library/ForeignLibrary.h"
#include "library/Importer.h"
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

// Runs library::importTracks on its own low-priority thread; progress and the result go to the browser's status
// line on the message thread.
class ImportJob final : public juce::Thread
{
public:
    ImportJob(library::Database& d, juce::Array<juce::File> in, juce::File art, ui::LibraryBrowser& b,
              std::optional<std::int64_t> parent)
        : juce::Thread("Library import")
        , db(d)
        , inputs(std::move(in))
        , artwork(std::move(art))
        , browser(&b)
        , playlistParent(parent)
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
};
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
        if (deck < deckStatus.size())
            deckStatus[deck] =
                t.file.getFileName() + " (" + juce::String(t.durationSeconds(), 1) + " s, " + t.formatName + ")";
    };
    loader.callbacks.onError = [this](engine::DeckId deck, const juce::File& file, const juce::String& error)
    {
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

void MainComponent::toggleKeyLock(engine::DeckId deck)
{
    const bool on = engine.readState().decks[deck].keyLock;
    engine.send(engine::SetKeyLock{deck, !on});
}

MainComponent::~MainComponent()
{
    loader.callbacks = {}; // the loader outlives this component; don't let late results call into it
    importJob = nullptr;   // stops a running import (between batches) before the browser goes
}

juce::File MainComponent::artworkFolder()
{
    return juce::File::getSpecialLocation(juce::File::windowsLocalAppData).getChildFile("Devil-DJ/cache/artwork");
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
    importJob =
        std::make_unique<ImportJob>(*libraryDb, filesOrFolders, artworkFolder(), *libraryBrowser, playlistParent);
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
    for (engine::DeckId d = 0; d < deckStatus.size(); ++d)
    {
        lines.add(juce::String("Deck ") + engine::deckLetter(d) + ": " + deckStatus[d]);
        const auto& s = state.decks[d];
        if (s.loaded)
        {
            const juce::String what = s.cuePreview ? "CUE" : s.playing ? "PLAY" : s.atEnd ? "END" : "pause";
            lines.add("   " + what + "  " + clock(s.positionSamples, state.sampleRate) + " / " +
                      clock(s.lengthSamples, state.sampleRate) + "  cue " + clock(s.cuePointSamples, state.sampleRate));
            lines.add(juce::String("   ") + (s.scratching ? "SCRATCH  " : "") + (s.keyLock ? "KEY LOCK  " : "") +
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
    const int deckHeight = std::min(250, top.getHeight());
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

} // namespace devildj::app
