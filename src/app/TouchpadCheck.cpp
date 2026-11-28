#include "app/TouchpadCheck.h"

namespace devildj::app::touchpad
{

std::optional<int> readSensitivity()
{
#if JUCE_WINDOWS
    const juce::String path = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad\\"
                              "AAPThreshold";
    if (!juce::WindowsRegistry::valueExists(path))
        return std::nullopt;
    const auto text = juce::WindowsRegistry::getValue(path).trim();
    if (text.isEmpty() || !text.containsOnly("0123456789"))
        return std::nullopt;
    return text.getIntValue();
#else
    return std::nullopt;
#endif
}

juce::String sensitivityName(int sensitivity)
{
    switch (sensitivity)
    {
        case 0: return "Most sensitive";
        case 1: return "High sensitivity";
        case 2: return "Medium sensitivity";
        case 3: return "Low sensitivity";
        default: return "unknown";
    }
}

bool mutesWhileTyping(std::optional<int> sensitivity) noexcept
{
    return sensitivity.has_value() && *sensitivity != 0;
}

bool showAtStart(std::optional<int> sensitivity, bool dismissed) noexcept
{
    return !dismissed && mutesWhileTyping(sensitivity);
}

juce::String tipText(std::optional<int> sensitivity)
{
    const juce::String how = "To change it: Windows Settings > Bluetooth & devices > Touchpad > Taps > "
                             "Touchpad sensitivity: Most sensitive.\n\nA USB mouse is never muted.";
    if (!sensitivity.has_value())
        return "Windows doesn't report a precision touchpad on this computer, so nothing mutes the pointer "
               "while you play pads from the keyboard.";
    if (!mutesWhileTyping(sensitivity))
        return "Your touchpad is set to Most sensitive: Windows doesn't mute it while keys are pressed, so the "
               "touchpad and the keyboard pads work together.";
    return "Windows mutes the touchpad for a moment after every key press (your setting: " +
           sensitivityName(*sensitivity) +
           "). While you play pads or other controls from the keyboard, the touchpad seems not to work.\n\n" + how;
}

} // namespace devildj::app::touchpad
