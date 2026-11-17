#include "MainWindow.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace devildj::app
{

class DevilDJApplication final : public juce::JUCEApplication
{
public:
    DevilDJApplication() = default;

    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String& /*commandLine*/) override
    {
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
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        settings = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }

    void systemRequestedQuit() override { quit(); }

    void anotherInstanceStarted(const juce::String& /*commandLine*/) override
    {
        if (mainWindow != nullptr)
            mainWindow->toFront(true);
    }

private:
    juce::LookAndFeel_V4 lookAndFeel;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<MainWindow> mainWindow;
};

} // namespace devildj::app

START_JUCE_APPLICATION(devildj::app::DevilDJApplication)
