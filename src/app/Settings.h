#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <memory>
#include <type_traits>

namespace devildj::app
{

// A named setting with a type and a default. Declare keys in SettingsKeys.h.
template <typename T>
struct SettingKey
{
    static_assert(std::is_same_v<T, bool> || std::is_same_v<T, int> || std::is_same_v<T, double> ||
                      std::is_same_v<T, juce::String>,
                  "Settings support bool, int, double and juce::String");

    const char* name;
    T defaultValue;
};

// App settings stored in %APPDATA%\Devil-DJ\settings.xml (ARCHITECTURE §8), wrapping juce::PropertiesFile.
//
// - Typed access: get(key) returns the stored value or the key's default; set(key, value) stores it.
// - Listeners hear about every change that actually changes a value (including reset()).
// - Saving: changes are written automatically shortly after they happen, and on destruction; save()
//   writes immediately.
//
// Use from the message thread.
class Settings
{
public:
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void settingChanged(const juce::String& keyName) = 0;
    };

    // The real settings file: %APPDATA%\Devil-DJ\settings.xml.
    Settings();

    // A specific file (tests, portable mode).
    explicit Settings(const juce::File& file);

    ~Settings();

    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    template <typename T>
    T get(const SettingKey<T>& key) const
    {
        auto& p = *properties;
        if constexpr (std::is_same_v<T, bool>)
            return p.getBoolValue(key.name, key.defaultValue);
        else if constexpr (std::is_same_v<T, int>)
            return p.getIntValue(key.name, key.defaultValue);
        else if constexpr (std::is_same_v<T, double>)
            return p.getDoubleValue(key.name, key.defaultValue);
        else
            return p.getValue(key.name, key.defaultValue);
    }

    template <typename T>
    void set(const SettingKey<T>& key, const T& value)
    {
        if (properties->containsKey(key.name) && get(key) == value)
            return;
        properties->setValue(key.name, juce::var(value));
        listeners.call([&](Listener& l) { l.settingChanged(key.name); });
    }

    // Removes the stored value so get() returns the default again.
    template <typename T>
    void reset(const SettingKey<T>& key)
    {
        if (!properties->containsKey(key.name))
            return;
        properties->removeValue(key.name);
        listeners.call([&](Listener& l) { l.settingChanged(key.name); });
    }

    template <typename T>
    bool isStored(const SettingKey<T>& key) const
    {
        return properties->containsKey(key.name);
    }

    // Writes pending changes now. Returns false if the file couldn't be written.
    bool save();

    juce::File getFile() const;

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

private:
    std::unique_ptr<juce::PropertiesFile> properties;
    juce::ListenerList<Listener> listeners;
};

} // namespace devildj::app
