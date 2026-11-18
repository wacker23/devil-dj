#pragma once

#include "Settings.h"

// Every app setting, with its type and default. Add new keys here (one place, so defaults are testable).
namespace devildj::app::keys
{

// Main window size/position as written by juce::ResizableWindow::getWindowStateAsString(). Empty = centre.
inline const SettingKey<juce::String> mainWindowState{"mainWindowState", {}};

// Audio device choice (type, device, sample rate, buffer size) as written by AudioDeviceService::getStateXml().
// Empty = use the system default device.
inline const SettingKey<juce::String> audioDeviceState{"audioDeviceState", {}};

// Output layout (engine::OutputLayout): 0 = stereo master only, 1 = 4-channel (cue on 3/4), 2 = split mono.
inline const SettingKey<int> outputLayout{"outputLayout", 0};

// Show the Debug menu (crash tests, log folder) in Release builds too. Debug builds always show it.
inline const SettingKey<bool> showDebugMenu{"showDebugMenu", false};

} // namespace devildj::app::keys
