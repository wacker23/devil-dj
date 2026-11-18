#include "AppMenu.h"
#include "AudioSettingsWindow.h"
#include "MainComponent.h"
#include "MainWindow.h"
#include "Settings.h"
#include "SettingsKeys.h"

#include "engine/AudioDeviceService.h"
#include "engine/AudioEngine.h"
#include "engine/BufferRegistry.h"
#include "engine/TrackLoader.h"
#include "util/CrashHandler.h"
#include "util/Log.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <filesystem>
#include <memory>

namespace devildj::app
{

namespace
{
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

        settings = std::make_unique<Settings>();
        util::logInfo("Settings: {}", settings->getFile().getFullPathName().toStdString());

        startAudio();

        auto content = std::make_unique<MainComponent>(*engine, *audio, *loader, *registry);
        mainComponent = content.get();
        mainWindow = std::make_unique<MainWindow>(getApplicationName(), *settings, std::move(content));
        mainComponent->grabKeyboardFocus();

        const bool showDebugMenu =
            kDebugBuild || commandLine.contains("--debug-menu") || settings->get(keys::showDebugMenu);
        menu = std::make_unique<AppMenu>(logDirectory(), showDebugMenu);
        menu->onAudioSettings = [this]
        {
            AudioSettingsComponent::Callbacks callbacks;
            callbacks.getLayout = [this] { return currentLayout(); };
            callbacks.setLayout = [this](engine::OutputLayout layout) { applyOutputLayout(layout, true); };
            showAudioSettingsWindow(*audio, *engine, std::move(callbacks), mainWindow.get());
        };
        menu->onToggleDebugOverlay = [this] { mainComponent->toggleDebugOverlay(); };
        menu->onToggleTestTone = [this] { mainComponent->toggleTestTone(); };
        menu->onLoadTrack = [this](int deck) { mainComponent->chooseTrackForDeck(static_cast<engine::DeckId>(deck)); };
        menu->onUnloadTrack = [this](int deck) { mainComponent->unloadDeck(static_cast<engine::DeckId>(deck)); };
        menu->onResetTempo = [this](int deck) { mainComponent->resetTempo(static_cast<engine::DeckId>(deck)); };
        menu->onTempoRange = [this](double range) { mainComponent->setTempoRange(range); };
        menu->onHighQualityResampling = [this](bool on) { mainComponent->setHighQualityResampling(on); };
        menu->onToggleKeyLock = [this](int deck) { mainComponent->toggleKeyLock(static_cast<engine::DeckId>(deck)); };
        mainWindow->setMenuBar(menu.get());
    }

    void shutdown() override
    {
        if (mainWindow != nullptr)
            mainWindow->setMenuBar(nullptr);
        mainWindow = nullptr; // also destroys mainComponent
        mainComponent = nullptr;
        menu = nullptr;
        loader = nullptr; // cancels and waits for running loads
        if (audio != nullptr)
            audio->setAudioCallback(nullptr); // stop calling the engine before it goes away
        audio = nullptr;
        engine = nullptr;
        registry = nullptr; // frees all track buffers (nothing uses them any more)
        settings = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);

        util::logInfo("Devil-DJ shut down cleanly");
        util::Log::stop();
    }

    void systemRequestedQuit() override { quit(); }

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

    // Opens the saved audio device (or the default one) and saves the user's choice whenever it changes.
    void startAudio()
    {
        engine = std::make_unique<engine::AudioEngine>();
        audio = std::make_unique<engine::AudioDeviceService>();
        audio->initialise(settings->get(keys::audioDeviceState));
        applyOutputLayout(currentLayout(), false);
        audio->setAudioCallback(engine.get());
        registry = std::make_unique<engine::BufferRegistry>();
        loader = std::make_unique<engine::TrackLoader>(*engine, *registry);
        audio->onDeviceChanged = [this]
        {
            if (const auto state = audio->getStateXml(); state.isNotEmpty())
                settings->set(keys::audioDeviceState, state);
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
    }

    juce::LookAndFeel_V4 lookAndFeel;
    std::unique_ptr<Settings> settings;
    std::unique_ptr<engine::AudioEngine> engine;
    std::unique_ptr<engine::AudioDeviceService> audio;
    std::unique_ptr<engine::BufferRegistry> registry;
    std::unique_ptr<engine::TrackLoader> loader;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<AppMenu> menu;
    MainComponent* mainComponent = nullptr; // owned by mainWindow
};

} // namespace devildj::app

START_JUCE_APPLICATION(devildj::app::DevilDJApplication)
