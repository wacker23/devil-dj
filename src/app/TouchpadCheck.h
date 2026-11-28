#pragma once

// The touchpad tip (owner request after the first manual test): Windows mutes a precision touchpad for a moment
// after every key press (palm rejection). Playing pads from the keyboard presses keys all the time, so the
// touchpad seems dead while mixing. The setting is Windows' own (Settings > Bluetooth & devices > Touchpad >
// Taps > Touchpad sensitivity); Devil-DJ can't override it - only spot it and say how to change it.

#include <juce_core/juce_core.h>

#include <optional>

namespace devildj::app::touchpad
{

// Windows' "Touchpad sensitivity" (the AAPThreshold value): 0 Most sensitive (never muted while typing),
// 1 High, 2 Medium (the default), 3 Low. nullopt: no precision touchpad setting (no touchpad, or not Windows).
std::optional<int> readSensitivity();

// The value as Windows names it ("Medium"); "unknown" for values Windows doesn't use.
juce::String sensitivityName(int sensitivity);

// True when Windows mutes the touchpad while keys are pressed (any setting but Most sensitive).
bool mutesWhileTyping(std::optional<int> sensitivity) noexcept;

// Shown at start once the touchpad is muted while typing, until the owner says not to.
bool showAtStart(std::optional<int> sensitivity, bool dismissed) noexcept;

// The tip's text for the current setting.
juce::String tipText(std::optional<int> sensitivity);

// Where Windows keeps the setting (Settings > Bluetooth & devices > Touchpad).
inline constexpr const char* kSettingsUri = "ms-settings:devices-touchpad";

} // namespace devildj::app::touchpad
