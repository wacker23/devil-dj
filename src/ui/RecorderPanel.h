#pragma once

#include "engine/AudioEngine.h"
#include "engine/Recorder.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace devildj::ui
{

// Mix recording (P3-16): REC (F9) starts / stops; the format (WAV / FLAC, 24-bit), what is recorded (the master, or
// the headphones), AUTO START (the file begins with the first sound), AUTO SPLIT (20 s of silence ends a file);
// the time, the file and a level meter of what is recorded. The app owns the recorder (onRecord) and the folder.
class RecorderPanel final : public juce::Component
{
public:
    static constexpr int kPreferredHeight = 96;

    struct Settings
    {
        bool flac = false;
        bool headphones = false;
        bool autoStart = false;
        bool autoSplit = false;
    };

    RecorderPanel(engine::AudioEngine& engine, engine::Recorder& recorder);
    ~RecorderPanel() override;

    std::function<void(bool start)> onRecord;      // REC pressed: start / stop
    std::function<void()> onOpenFolder;
    std::function<void(const Settings&)> onSettingsChanged;
    void setSettings(const Settings& s);
    const Settings& settings() const noexcept { return current; }

    void refresh();
    void resized() override;
    void paint(juce::Graphics& g) override;

    juce::TextButton& recButton() noexcept { return rec; }
    juce::TextButton& formatButton() noexcept { return format; }
    juce::TextButton& sourceButton() noexcept { return source; }
    juce::TextButton& autoStartButton() noexcept { return autoStart; }
    juce::TextButton& autoSplitButton() noexcept { return autoSplit; }
    juce::String statusText() const;

private:
    void changed();

    engine::AudioEngine& engine;
    engine::Recorder& recorder;
    Settings current;
    juce::TextButton rec{"REC"}, format{"WAV"}, source{"MASTER"}, autoStart{"AUTO START"}, autoSplit{"AUTO SPLIT"},
        folder{"FOLDER"};
    juce::Rectangle<int> infoArea, meterArea;
    juce::VBlankAttachment vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RecorderPanel)
};

} // namespace devildj::ui
