#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace devildj::ui
{

// Automix (P3-18): AUTOMIX on / off (F10), QUEUE (the browser's selected tracks, in their order), REPEAT, RANDOM,
// the transition's length (8 / 16 / 32 beats) and style; what plays now and next. The app runs app::Automix.
class AutomixPanel final : public juce::Component
{
public:
    static constexpr int kPreferredHeight = 96;
    struct Settings
    {
        bool repeat = false, random = false;
        int beats = 16;
        int style = 0; // 0 blend, 1 filter sweep, 2 echo out
    };

    AutomixPanel();
    std::function<void()> onToggle;   // AUTOMIX
    std::function<void()> onQueue;    // QUEUE: take the browser's selection
    std::function<void(const Settings&)> onSettingsChanged;
    void setSettings(const Settings& s);
    const Settings& settings() const noexcept { return current; }
    // What to show: on or off, and a line about the queue ("3 / 10  Now: ...  Next: ...").
    void setStatus(bool on, const juce::String& text);

    void resized() override;
    void paint(juce::Graphics& g) override;

    juce::TextButton& automixButton() noexcept { return automix; }
    juce::TextButton& queueButton() noexcept { return queue; }
    juce::TextButton& repeatButton() noexcept { return repeat; }
    juce::TextButton& randomButton() noexcept { return random; }
    juce::TextButton& beatsButton() noexcept { return beats; }
    juce::TextButton& styleButton() noexcept { return style; }
    const juce::String& statusText() const noexcept { return status; }

private:
    void changed();
    Settings current;
    juce::TextButton automix{"AUTOMIX"}, queue{"QUEUE"}, repeat{"REPEAT"}, random{"RANDOM"}, beats{"16 BEATS"},
        style{"BLEND"};
    juce::String status{"Queue some tracks: select them in the browser, then QUEUE."};
    juce::Rectangle<int> statusArea;
};

} // namespace devildj::ui
