#include "AppMenu.h"
#include "AudioSettingsWindow.h"
#include "LibraryBackup.h"
#include "MainComponent.h"
#include "MainWindow.h"
#include "Settings.h"
#include "SettingsKeys.h"

#include "engine/AudioDeviceService.h"
#include "engine/AudioEngine.h"
#include "engine/BufferRegistry.h"
#include "engine/CueDevice.h"
#include "engine/TrackLoader.h"
#include "library/Database.h"
#include "util/CrashHandler.h"
#include "util/Log.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <filesystem>
#include <memory>

namespace devildj::app
{

namespace
{
// A one-off job on its own low-priority thread (library backups).
class Worker final : public juce::Thread
{
public:
    Worker(const juce::String& name, std::function<void()> j)
        : juce::Thread(name)
        , job(std::move(j))
    {
    }
    ~Worker() override { stopThread(60000); } // a backup of a big library can take a few seconds
    void run() override { job(); }

private:
    std::function<void()> job;
};

#if JUCE_DEBUG
constexpr bool kDebugBuild = true;
#else
constexpr bool kDebugBuild = false;
#endif
} // namespace

class DevilDJApplication final : public juce::JUCEApplication
{
public:
    DevilDJApplication() = default;

    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String& commandLine) override
    {
        startDiagnostics();

        lookAndFeel.setColourScheme(juce::LookAndFeel_V4::getDarkColourScheme());
        juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);

        // A restore staged last time (Options > Restore library from a backup...) goes in before anything reads
        // the library or the settings (P2-11).
        const auto restored = backup::applyPendingRestore(libraryFile(), Settings::defaultFile());
        if (restored.isNotEmpty())
            util::logInfo("{}", restored.toStdString());

        settings = std::make_unique<Settings>();
        util::logInfo("Settings: {}", settings->getFile().getFullPathName().toStdString());

        startAudio();
        const juce::String libraryProblem = openLibrary();

        auto content = std::make_unique<MainComponent>(*engine, *audio, *loader, *registry, database.get(),
                                                       libraryProblem);
        mainComponent = content.get();
        mainWindow = std::make_unique<MainWindow>(getApplicationName(), *settings, std::move(content));
        mainComponent->grabKeyboardFocus();
        if (const juce::File folder(settings->get(keys::browserFolder)); folder.isDirectory())
            mainComponent->browser().setFolder(folder);
        mainComponent->browser().onFolderChanged = [this](const juce::File& folder)
        { settings->set(keys::browserFolder, folder.getFullPathName()); };
        mainComponent->libraryView().setColumnLayout(settings->get(keys::libraryColumns));
        mainComponent->libraryView().onColumnLayoutChanged = [this](const juce::String& layout)
        { settings->set(keys::libraryColumns, layout); };

        const bool showDebugMenu =
            kDebugBuild || commandLine.contains("--debug-menu") || settings->get(keys::showDebugMenu);
        menu = std::make_unique<AppMenu>(logDirectory(), showDebugMenu);
        menu->onImportFolder = [this] { mainComponent->importToLibrary({}); };
        menu->onBackupNow = [this] { backupNow(); };
        menu->onRestoreBackup = [this] { chooseRestore(); };
        menu->onOpenBackups = [] { backup::defaultFolder().createDirectory(); backup::defaultFolder().startAsProcess(); };
        const bool camelot = settings->get(keys::keyNotation) == 1;
        menu->setKeyNotation(camelot ? 1 : 0);
        mainComponent->setKeyNotation(camelot ? util::KeyNotation::Camelot : util::KeyNotation::Musical);
        menu->setQuantize(settings->get(keys::quantize), settings->get(keys::quantizeEighths));
        mainComponent->setQuantize(settings->get(keys::quantize), settings->get(keys::quantizeEighths) / 8.0);
        menu->onQuantize = [this](bool on, int eighths)
        {
            settings->set(keys::quantize, on);
            settings->set(keys::quantizeEighths, eighths);
            mainComponent->setQuantize(on, eighths / 8.0);
        };
        mainComponent->onQuantizeChanged = [this](bool on, double beats)
        {
            const int eighths = static_cast<int>(std::lround(beats * 8.0));
            settings->set(keys::quantize, on);
            settings->set(keys::quantizeEighths, eighths);
            menu->setQuantize(on, eighths);
        };
        // The mixer as it was left (P2 follow-up).
        {
            const double cueMix = std::clamp(settings->get(keys::headphoneMix), 0.0, 1.0);
            const double level = std::clamp(settings->get(keys::headphoneLevelDb), -100.0, 6.0);
            const int curve = settings->get(keys::crossfaderCurve), assignA = settings->get(keys::crossfaderAssignA),
                      assignB = settings->get(keys::crossfaderAssignB);
            mainComponent->setCueMix(cueMix);
            mainComponent->setHeadphoneLevel(level);
            mainComponent->setCrossfaderCurve(std::clamp(curve, 0, 2));
            mainComponent->setCrossfaderAssign(0, std::clamp(assignA, 0, 2));
            mainComponent->setCrossfaderAssign(1, std::clamp(assignB, 0, 2));
            menu->setMixerState(cueMix, level, curve, assignA, assignB);
        }
        {
            const int style = std::clamp(settings->get(keys::pauseStyle), 0, 2);
            const double brake = std::clamp(settings->get(keys::brakeSeconds), 0.05, 10.0);
            const double spin = std::clamp(settings->get(keys::spinBackSeconds), 0.1, 10.0);
            menu->setStopStyle(style, brake, spin);
            mainComponent->setStopStyle(style, brake, spin);
        }
        menu->onStopStyle = [this](int style, double brake, double spin)
        {
            settings->set(keys::pauseStyle, style);
            settings->set(keys::brakeSeconds, brake);
            settings->set(keys::spinBackSeconds, spin);
            mainComponent->setStopStyle(style, brake, spin);
        };
        mainComponent->setColorFx(settings->get(keys::colorFx)); // P3-10
        mainComponent->onColorFxChanged = [this](int type) { settings->set(keys::colorFx, type); };
        menu->setKeySyncCompatible(settings->get(keys::keySyncCompatible));
        mainComponent->setKeySyncCompatible(settings->get(keys::keySyncCompatible));
        menu->onKeySyncCompatible = [this](bool on)
        {
            settings->set(keys::keySyncCompatible, on);
            mainComponent->setKeySyncCompatible(on);
        };
        menu->setAutoGain(settings->get(keys::autoGain), settings->get(keys::autoGainTarget));
        mainComponent->setAutoGain(settings->get(keys::autoGain), settings->get(keys::autoGainTarget));
        menu->onAutoGain = [this](bool on, int target)
        {
            settings->set(keys::autoGain, on);
            settings->set(keys::autoGainTarget, target);
            mainComponent->setAutoGain(on, target);
        };
        menu->onKeyNotation = [this](int notation)
        {
            settings->set(keys::keyNotation, notation);
            mainComponent->setKeyNotation(notation == 1 ? util::KeyNotation::Camelot : util::KeyNotation::Musical);
        };
        menu->onImportPlaylist = [this] { mainComponent->importPlaylistFile(); };
        menu->onImportLibrary = [this] { mainComponent->importOtherLibrary(); };
        menu->onCheckMissing = [this] { mainComponent->checkMissingFiles(false); };
        menu->onRelocate = [this] { mainComponent->relocateMissingFiles(); };
        menu->onAudioSettings = [this]
        {
            AudioSettingsComponent::Callbacks callbacks;
            callbacks.getLayout = [this] { return currentLayout(); };
            callbacks.setLayout = [this](engine::OutputLayout layout) { applyOutputLayout(layout, true); };
            callbacks.cueDevice = cueDevice.get();
            callbacks.setCueDevice = [this](const juce::String& name)
            {
                settings->set(keys::cueDevice, name);
                return openCueDevice();
            };
            showAudioSettingsWindow(*audio, *engine, std::move(callbacks), mainWindow.get());
        };
        menu->onToggleDebugOverlay = [this] { mainComponent->toggleDebugOverlay(); };
        menu->onToggleTestTone = [this] { mainComponent->toggleTestTone(); };
        menu->onLoadTrack = [this](int deck) { mainComponent->chooseTrackForDeck(static_cast<engine::DeckId>(deck)); };
        menu->onUnloadTrack = [this](int deck) { mainComponent->unloadDeck(static_cast<engine::DeckId>(deck)); };
        menu->onResetTempo = [this](int deck) { mainComponent->resetTempo(static_cast<engine::DeckId>(deck)); };
        menu->onTempoRange = [this](double range) { mainComponent->setTempoRange(range); };
        menu->onHighQualityResampling = [this](bool on) { mainComponent->setHighQualityResampling(on); };
        menu->onReloadKeymap = [this] { mainComponent->reloadKeymap(); };
        menu->onSoakTest = [this](double minutes)
        {
            if (minutes <= 0.0)
                mainComponent->stopSoak();
            else
                mainComponent->startSoak(minutes, false);
        };
        startSoakFromCommandLine(commandLine);
        importFromCommandLine(commandLine);
        menu->onOpenKeymapFolder = [this] { mainComponent->openKeymapFolder(); };
        menu->onToggleKeyLock = [this](int deck) { mainComponent->toggleKeyLock(static_cast<engine::DeckId>(deck)); };
        menu->onIsolator = [this](bool on) { mainComponent->setIsolator(on); };
        menu->onCrossfaderCurve = [this](int curve) { mainComponent->setCrossfaderCurve(curve); };
        menu->onCrossfaderAssign = [this](int deck, int assign) { mainComponent->setCrossfaderAssign(deck, assign); };
        menu->onCueMix = [this](double mix) { mainComponent->setCueMix(mix); };
        menu->onHeadphoneLevel = [this](double db) { mainComponent->setHeadphoneLevel(db); };
        mainWindow->setMenuBar(menu.get());

        if (restored.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Library restored", restored);
        startAutomaticBackup();
    }

    // P2-11: once a day, in the background, a backup into the backups folder (the newest 7 automatic ones kept).
    void startAutomaticBackup()
    {
        if (database == nullptr)
            return;
        settings->save();
        auto* db = database.get();
        const auto settingsFile = settings->getFile();
        backupThread = std::make_unique<Worker>(
            "Library backup",
            [db, settingsFile]
            {
                juce::String error;
                const auto zip = backup::runAutomatic(*db, settingsFile, backup::defaultFolder(),
                                                      juce::Time::getCurrentTime(), &error);
                if (zip != juce::File())
                    util::logInfo("Library backup: {}", zip.getFullPathName().toStdString());
                else if (error.isNotEmpty())
                    util::logWarning("Library backup failed: {}", error.toStdString());
            });
        backupThread->startThread(juce::Thread::Priority::low);
    }

    bool backupBusy() const { return backupThread != nullptr && backupThread->isThreadRunning(); }

    void tellUser(const juce::String& text)
    {
        juce::MessageManager::callAsync(
            [this, text]
            {
                if (mainComponent != nullptr)
                    mainComponent->libraryView().setStatus(text);
            });
    }

    void backupNow()
    {
        if (database == nullptr || backupBusy())
            return;
        const auto folder = backup::defaultFolder();
        folder.createDirectory();
        fileChooser = std::make_unique<juce::FileChooser>(
            "Back up the library to",
            folder.getChildFile("backup-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S") + ".zip"), "*.zip");
        fileChooser->launchAsync(
            juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
            [this](const juce::FileChooser& fc)
            {
                auto zip = fc.getResult();
                if (zip == juce::File() || database == nullptr || backupBusy())
                    return;
                if (!zip.hasFileExtension(".zip"))
                    zip = zip.withFileExtension(".zip");
                settings->save();
                auto* db = database.get();
                const auto settingsFile = settings->getFile();
                tellUser("Backing up the library...");
                backupThread = std::make_unique<Worker>(
                    "Library backup",
                    [this, db, settingsFile, zip]
                    {
                        const auto r = backup::create(*db, settingsFile, zip);
                        util::logInfo("Library backup to {}: {}", zip.getFullPathName().toStdString(),
                                      r.wasOk() ? "done" : r.getErrorMessage().toStdString());
                        tellUser(r.wasOk() ? "Backed up to " + zip.getFullPathName()
                                           : "Backup failed: " + r.getErrorMessage());
                    });
                backupThread->startThread(juce::Thread::Priority::low);
            });
    }

    void chooseRestore()
    {
        if (backupBusy())
            return;
        fileChooser = std::make_unique<juce::FileChooser>("Restore the library from a backup", backup::defaultFolder(),
                                                          "*.zip");
        fileChooser->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc)
            {
                const auto zip = fc.getResult();
                if (!zip.existsAsFile() || backupBusy())
                    return;
                tellUser("Checking " + zip.getFileName() + "...");
                const auto settingsFile = settings->getFile();
                backupThread = std::make_unique<Worker>(
                    "Library restore",
                    [this, zip, settingsFile]
                    {
                        const auto r = backup::stageRestore(zip, libraryFile(), settingsFile);
                        util::logInfo("Library restore from {}: {}", zip.getFullPathName().toStdString(),
                                      r.wasOk() ? "staged" : r.getErrorMessage().toStdString());
                        juce::MessageManager::callAsync(
                            [r, zip]
                            {
                                if (r.failed())
                                {
                                    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                           "Can't restore", r.getErrorMessage());
                                    return;
                                }
                                juce::AlertWindow::showOkCancelBox(
                                    juce::MessageBoxIconType::QuestionIcon, "Restore the library",
                                    zip.getFileName() + " is ready. Devil-DJ restores it the next time it starts "
                                                        "(your current library is kept as a copy).\n\nQuit now?",
                                    "Quit now", "Later", nullptr,
                                    juce::ModalCallbackFunction::create(
                                        [](int result)
                                        {
                                            if (result == 1)
                                                juce::JUCEApplication::getInstance()->systemRequestedQuit();
                                        }));
                            });
                    });
                backupThread->startThread(juce::Thread::Priority::low);
            });
    }

    void shutdown() override
    {
        saveMixerState();
        if (mainWindow != nullptr)
            mainWindow->setMenuBar(nullptr);
        backupThread = nullptr; // waits for a running backup before the library closes
        fileChooser = nullptr;
        mainWindow = nullptr; // also destroys mainComponent (and stops a running import)
        mainComponent = nullptr;
        database = nullptr; // runs the queued writes, then closes the library
        menu = nullptr;
        loader = nullptr; // cancels and waits for running loads
        if (audio != nullptr)
            audio->setAudioCallback(nullptr); // stop calling the engine before it goes away
        cueDevice = nullptr; // its callback reads the engine's buffer
        audio = nullptr;
        engine = nullptr;
        registry = nullptr; // frees all track buffers (nothing uses them any more)
        settings = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);

        util::logInfo("Devil-DJ shut down cleanly");
        util::Log::stop();
    }

    void systemRequestedQuit() override { quit(); }

    // --soak=MIN [--soak-silent] [--soak-quit] [--soak-driver=NAME] [--soak-buffer=N] [--soak-tracks="a.mp3|b.mp3"]
    // (P1-20)
    void startSoakFromCommandLine(const juce::String& commandLine)
    {
        const auto args = juce::StringArray::fromTokens(commandLine, true);
        double minutes = 0.0;
        bool silent = false, quit = false;
        int buffer = 0;
        juce::String driver;
        juce::StringArray tracks;
        for (auto arg : args)
        {
            arg = arg.unquoted();
            if (arg.startsWith("--soak="))
                minutes = arg.fromFirstOccurrenceOf("=", false, false).getDoubleValue();
            else if (arg == "--soak-silent")
                silent = true;
            else if (arg.startsWith("--soak-driver="))
                driver = arg.fromFirstOccurrenceOf("=", false, false).unquoted();
            else if (arg.startsWith("--soak-buffer="))
                buffer = arg.fromFirstOccurrenceOf("=", false, false).getIntValue();
            else if (arg == "--soak-quit")
                quit = true;
            else if (arg.startsWith("--soak-tracks="))
                tracks =
                    juce::StringArray::fromTokens(arg.fromFirstOccurrenceOf("=", false, false).unquoted(), "|", "");
        }
        if (minutes <= 0.0)
            return;
        if (driver.isNotEmpty()) // e.g. "Windows Audio (Low Latency Mode)"; saved like a settings change
            if (const auto error = audio->setDeviceType(driver); error.isNotEmpty())
                util::logWarning("Soak test: driver {} not available: {}", driver.toStdString(), error.toStdString());
        if (buffer > 0) // saved like a change on the settings page
            if (const auto error = audio->setBufferSize(buffer); error.isNotEmpty())
                util::logWarning("Soak test: buffer {} not available: {}", buffer, error.toStdString());
        for (int d = 0; d < std::min(2, tracks.size()); ++d)
            mainComponent->loadFile(static_cast<engine::DeckId>(d), juce::File(tracks[d]));
        util::logInfo("Soak test requested on the command line: {} min", minutes);
        mainComponent->startSoakWhenLoaded(minutes, silent, quit);
    }

    // --import="C:\Music\Folder" (P2-04): adds a folder (or file) to the library at start, like IMPORT.
    void importFromCommandLine(const juce::String& commandLine)
    {
        juce::Array<juce::File> inputs;
        for (auto arg : juce::StringArray::fromTokens(commandLine, true))
        {
            arg = arg.unquoted();
            if (arg.startsWith("--import="))
                if (const juce::File f(arg.fromFirstOccurrenceOf("=", false, false).unquoted()); f.exists())
                    inputs.add(f);
        }
        if (!inputs.isEmpty())
            mainComponent->importToLibrary(inputs);
    }

    void anotherInstanceStarted(const juce::String& /*commandLine*/) override
    {
        if (mainWindow != nullptr)
            mainWindow->toFront(true);
    }

private:
    // %LOCALAPPDATA%\Devil-DJ\logs: log files and crash dumps (ARCHITECTURE §8).
    static juce::File logDirectory()
    {
        return juce::File::getSpecialLocation(juce::File::windowsLocalAppData)
            .getChildFile("Devil-DJ")
            .getChildFile("logs");
    }

    void startDiagnostics()
    {
        const auto dir = std::filesystem::path(logDirectory().getFullPathName().toWideCharPointer());

        util::LogOptions logOptions;
        logOptions.directory = dir;
        const bool logging = util::Log::start(logOptions);
        const bool crashHandler = util::installCrashHandler({dir});

        util::logInfo("Devil-DJ {} starting ({} build, {})", getApplicationVersion().toStdString(),
                      kDebugBuild ? "Debug" : "Release", juce::SystemStats::getOperatingSystemName().toStdString());
        if (!logging || !crashHandler)
            util::logWarning("Diagnostics incomplete: log={} crashHandler={}", logging, crashHandler);
    }

    // The library (ARCHITECTURE §8): %APPDATA%\Devil-DJ\library.db. Returns a problem to show, or empty.
    static juce::File libraryFile()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Devil-DJ")
            .getChildFile("library.db");
    }

    juce::String openLibrary()
    {
        const auto file = libraryFile();
        auto opened = library::Database::open(std::filesystem::path(file.getFullPathName().toWideCharPointer()));
        if (!opened)
        {
            util::logError("Library: can't open {}: {}", file.getFullPathName().toStdString(), opened.error());
            return juce::String::fromUTF8(opened.error().c_str());
        }
        database = std::move(opened).value();
        util::logInfo("Library: {} (schema {})", file.getFullPathName().toStdString(), database->schemaVersion());
        return {};
    }

    // Opens the saved audio device (or the default one) and saves the user's choice whenever it changes.
    void startAudio()
    {
        engine = std::make_unique<engine::AudioEngine>();
        audio = std::make_unique<engine::AudioDeviceService>();
        audio->initialise(settings->get(keys::audioDeviceState));
        cueDevice = std::make_unique<engine::CueDevice>(*audio, engine->separateCueOutput());
        applyOutputLayout(currentLayout(), false);
        audio->setAudioCallback(engine.get());
        registry = std::make_unique<engine::BufferRegistry>();
        engine::TrackLoader::Options loaderOptions;
        loaderOptions.waveformCache = MainComponent::cacheFolder(); // P2-17
        loader = std::make_unique<engine::TrackLoader>(*engine, *registry, loaderOptions);
        audio->onDeviceChanged = [this]
        {
            if (const auto state = audio->getStateXml(); state.isNotEmpty())
                settings->set(keys::audioDeviceState, state);
            // Another master device (or driver): the headphone device follows.
            if (currentLayout() == engine::OutputLayout::SeparateCueDevice)
                openCueDevice();
        };
    }

    engine::OutputLayout currentLayout() const
    {
        const int value = settings->get(keys::outputLayout);
        if (value < 0 || value >= engine::kNumOutputLayouts)
            return engine::OutputLayout::StereoMaster;
        return static_cast<engine::OutputLayout>(value);
    }

    // Tells the engine, opens the outputs the layout needs, optionally saves the choice.
    void applyOutputLayout(engine::OutputLayout layout, bool save)
    {
        if (save)
            settings->set(keys::outputLayout, static_cast<int>(layout));
        engine->send(engine::SetOutputLayout{layout});
        if (const auto error = audio->setOutputChannelCount(engine::outputChannelsFor(layout)); error.isNotEmpty())
            util::logWarning("Audio: {}", error.toStdString());
        util::logInfo("Audio: output layout {}", engine::outputLayoutName(layout));
        if (layout == engine::OutputLayout::SeparateCueDevice)
            openCueDevice();
        else if (cueDevice != nullptr)
            cueDevice->close();
    }

    // The mixer as it is now (knobs and menus both), for the next start.
    void saveMixerState()
    {
        if (engine == nullptr || settings == nullptr)
            return;
        const auto& s = engine->readState();
        if (s.blockCounter == 0) // the engine never ran (no sound card): it holds no choices of the user's
            return;
        settings->set(keys::headphoneMix, static_cast<double>(s.cueMix));
        settings->set(keys::headphoneLevelDb, static_cast<double>(s.headphoneLevelDb));
        settings->set(keys::crossfaderCurve, static_cast<int>(s.crossfaderCurve));
        settings->set(keys::crossfaderAssignA, static_cast<int>(s.channels[0].crossfaderAssign));
        settings->set(keys::crossfaderAssignB, static_cast<int>(s.channels[1].crossfaderAssign));
    }

    // P2-25: the saved headphone device, or the first other output of the driver. Error, or empty.
    juce::String openCueDevice()
    {
        if (cueDevice == nullptr)
            return "no audio";
        auto name = settings->get(keys::cueDevice);
        const auto available = cueDevice->availableDevices();
        if (!available.contains(name))
            name = available.isEmpty() ? juce::String() : available[0];
        const auto error = cueDevice->open(name);
        if (error.isNotEmpty())
            util::logWarning("Audio: headphone device: {}", error.toStdString());
        return error;
    }

    juce::LookAndFeel_V4 lookAndFeel;
    std::unique_ptr<Settings> settings;
    std::unique_ptr<engine::AudioEngine> engine;
    std::unique_ptr<engine::AudioDeviceService> audio;
    std::unique_ptr<engine::CueDevice> cueDevice; // P2-25; before the engine goes (it reads the engine's buffer)
    std::unique_ptr<engine::BufferRegistry> registry;
    std::unique_ptr<engine::TrackLoader> loader;
    std::unique_ptr<library::Database> database;
    std::unique_ptr<juce::Thread> backupThread;
    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<AppMenu> menu;
    MainComponent* mainComponent = nullptr; // owned by mainWindow
};

} // namespace devildj::app

START_JUCE_APPLICATION(devildj::app::DevilDJApplication)
