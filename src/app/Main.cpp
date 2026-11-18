#include "AppMenu.h"
#include "AudioSettingsWindow.h"
#include "MainComponent.h"
#include "MainWindow.h"
#include "Settings.h"
#include "SettingsKeys.h"

#include "engine/AudioDeviceService.h"
#include "engine/AudioEngine.h"
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

        auto content = std::make_unique<MainComponent>(*engine, *audio);
        mainComponent = content.get();
        mainWindow = std::make_unique<MainWindow>(getApplicationName(), *settings, std::move(content));
        mainComponent->grabKeyboardFocus();

        const bool showDebugMenu =
            kDebugBuild || commandLine.contains("--debug-menu") || settings->get(keys::showDebugMenu);
        menu = std::make_unique<AppMenu>(logDirectory(), showDebugMenu);
        menu->onAudioSettings = [this] { showAudioSettingsWindow(*audio, mainWindow.get()); };
        menu->onToggleDebugOverlay = [this] { mainComponent->toggleDebugOverlay(); };
        menu->onToggleTestTone = [this] { mainComponent->toggleTestTone(); };
        mainWindow->setMenuBar(menu.get());
    }

    void shutdown() override
    {
        if (mainWindow != nullptr)
            mainWindow->setMenuBar(nullptr);
        mainWindow = nullptr; // also destroys mainComponent
        mainComponent = nullptr;
        menu = nullptr;
        if (audio != nullptr)
            audio->setAudioCallback(nullptr); // stop calling the engine before it goes away
        audio = nullptr;
        engine = nullptr;
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
        audio->setAudioCallback(engine.get());
        audio->onDeviceChanged = [this]
        {
            if (const auto state = audio->getStateXml(); state.isNotEmpty())
                settings->set(keys::audioDeviceState, state);
        };
    }

    juce::LookAndFeel_V4 lookAndFeel;
    std::unique_ptr<Settings> settings;
    std::unique_ptr<engine::AudioEngine> engine;
    std::unique_ptr<engine::AudioDeviceService> audio;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<AppMenu> menu;
    MainComponent* mainComponent = nullptr; // owned by mainWindow
};

} // namespace devildj::app

START_JUCE_APPLICATION(devildj::app::DevilDJApplication)
