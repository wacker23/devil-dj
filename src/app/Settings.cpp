#include "Settings.h"

namespace devildj::app
{

namespace
{
constexpr int kSaveDelayMs = 500;

juce::PropertiesFile::Options makeOptions()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "settings";
    options.folderName = "Devil-DJ";
    options.filenameSuffix = "xml";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    options.osxLibrarySubFolder = "Application Support";
    options.millisecondsBeforeSaving = kSaveDelayMs;
    return options;
}
} // namespace

Settings::Settings()
    : properties(std::make_unique<juce::PropertiesFile>(makeOptions()))
{
}

juce::File Settings::defaultFile()
{
    return makeOptions().getDefaultFile();
}

Settings::Settings(const juce::File& file)
    : properties(std::make_unique<juce::PropertiesFile>(file, makeOptions()))
{
}

Settings::~Settings()
{
    properties->saveIfNeeded();
}

bool Settings::save()
{
    return properties->saveIfNeeded();
}

juce::File Settings::getFile() const
{
    return properties->getFile();
}

void Settings::addListener(Listener* listener)
{
    listeners.add(listener);
}

void Settings::removeListener(Listener* listener)
{
    listeners.remove(listener);
}

} // namespace devildj::app
