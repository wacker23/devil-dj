#include "DebugMenu.h"
#include "MainWindow.h"

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

        // Temporary settings file until P0-11 wraps it in app/Settings. Location: %APPDATA%\Devil-DJ\settings.xml
        juce::PropertiesFile::Options options;
        options.applicationName = "settings";
        options.folderName = "Devil-DJ";
        options.filenameSuffix = "xml";
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        options.osxLibrarySubFolder = "Application Support";
        settings = std::make_unique<juce::PropertiesFile>(options);

        mainWindow = std::make_unique<MainWindow>(getApplicationName(), *settings);

        const bool showDebugMenu = kDebugBuild || commandLine.contains("--debug-menu");
        if (showDebugMenu)
        {
            debugMenu = std::make_unique<DebugMenu>(logDirectory());
            mainWindow->setMenuBar(debugMenu.get());
        }
    }

    void shutdown() override
    {
        if (mainWindow != nullptr)
            mainWindow->setMenuBar(nullptr);
        mainWindow = nullptr;
        debugMenu = nullptr;
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

    juce::LookAndFeel_V4 lookAndFeel;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<DebugMenu> debugMenu;
};

} // namespace devildj::app

START_JUCE_APPLICATION(devildj::app::DevilDJApplication)
