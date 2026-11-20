#include "Settings.h"
#include "SettingsKeys.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using devildj::app::SettingKey;
using devildj::app::Settings;
namespace keys = devildj::app::keys;

namespace
{
const SettingKey<bool> testBool{"test.bool", true};
const SettingKey<int> testInt{"test.int", 256};
const SettingKey<double> testDouble{"test.double", 0.5};
const SettingKey<juce::String> testString{"test.string", "default"};

// A settings file in a fresh temporary folder, deleted afterwards.
struct TempSettingsFile
{
    TempSettingsFile()
        : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getNonexistentChildFile("devildj-settings-test", {}, false))
    {
        dir.createDirectory();
    }
    ~TempSettingsFile() { dir.deleteRecursively(); }

    juce::File file() const { return dir.getChildFile("settings.xml"); }

    juce::File dir;
};

struct RecordingListener : Settings::Listener
{
    void settingChanged(const juce::String& keyName) override { changes.add(keyName); }
    juce::StringArray changes;
};
} // namespace

TEST_CASE("Settings returns defaults when nothing is stored", "[app][Settings]")
{
    TempSettingsFile tmp;
    Settings settings(tmp.file());

    CHECK(settings.get(testBool) == true);
    CHECK(settings.get(testInt) == 256);
    CHECK(settings.get(testDouble) == 0.5);
    CHECK(settings.get(testString) == "default");
    CHECK_FALSE(settings.isStored(testInt));
}

TEST_CASE("Settings app keys have the documented defaults", "[app][Settings]")
{
    TempSettingsFile tmp;
    Settings settings(tmp.file());

    CHECK(settings.get(keys::mainWindowState).isEmpty()); // empty = centre the window
    CHECK(settings.get(keys::showDebugMenu) == false);
    CHECK(settings.get(keys::outputLayout) == 0);          // stereo master only
    CHECK(settings.get(keys::keyNotation) == 0);           // musical keys (P2-15)
    CHECK(settings.get(keys::autoGain));                   // Auto Gain on (P2-16)
    CHECK(settings.get(keys::autoGainTarget) == -10);      // LUFS
    CHECK(settings.get(keys::audioDeviceState).isEmpty()); // system default device
}

TEST_CASE("Settings values survive a restart", "[app][Settings]")
{
    TempSettingsFile tmp;
    {
        Settings settings(tmp.file());
        settings.set(testBool, false);
        settings.set(testInt, 512);
        settings.set(testDouble, 0.125);
        settings.set(testString, juce::String::fromUTF8(reinterpret_cast<const char*>(u8"데빌 DJ")));
    } // destructor saves

    REQUIRE(tmp.file().existsAsFile());

    Settings reopened(tmp.file());
    CHECK(reopened.get(testBool) == false);
    CHECK(reopened.get(testInt) == 512);
    CHECK(reopened.get(testDouble) == 0.125);
    CHECK(reopened.get(testString) == juce::String::fromUTF8(reinterpret_cast<const char*>(u8"데빌 DJ")));
}

TEST_CASE("Settings save writes immediately", "[app][Settings]")
{
    TempSettingsFile tmp;
    Settings settings(tmp.file());
    settings.set(testInt, 1024);
    REQUIRE(settings.save());

    Settings other(tmp.file()); // reads the file while the first instance is still alive
    CHECK(other.get(testInt) == 1024);
}

TEST_CASE("Settings listeners hear real changes only", "[app][Settings]")
{
    TempSettingsFile tmp;
    Settings settings(tmp.file());
    RecordingListener listener;
    settings.addListener(&listener);

    settings.set(testInt, 128);
    settings.set(testInt, 128); // same value: no notification
    settings.set(testString, juce::String("x"));
    settings.reset(testInt);
    settings.reset(testInt); // already default: no notification

    CHECK(listener.changes == juce::StringArray{"test.int", "test.string", "test.int"});
    CHECK(settings.get(testInt) == 256);

    settings.removeListener(&listener);
    settings.set(testBool, false);
    CHECK(listener.changes.size() == 3);
}

TEST_CASE("Settings setting the default value explicitly stores it", "[app][Settings]")
{
    TempSettingsFile tmp;
    Settings settings(tmp.file());
    RecordingListener listener;
    settings.addListener(&listener);

    settings.set(testInt, 256); // equals the default, but wasn't stored yet
    CHECK(settings.isStored(testInt));
    CHECK(listener.changes.size() == 1);
}

TEST_CASE("Settings real file lives in %APPDATA%\\Devil-DJ\\settings.xml", "[app][Settings]")
{
    // Only builds the path; doesn't write anything to the real file.
    juce::PropertiesFile::Options options;
    options.applicationName = "settings";
    options.folderName = "Devil-DJ";
    options.filenameSuffix = "xml";
    const auto expected = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                              .getChildFile("Devil-DJ")
                              .getChildFile("settings.xml");
    CHECK(options.getDefaultFile() == expected);
}
