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

// Output layout (engine::OutputLayout): 0 = stereo master only, 1 = 4-channel (cue on 3/4), 2 = split mono,
// 3 = the cue on a separate headphone device (P2-25).
inline const SettingKey<int> outputLayout{"outputLayout", 0};

// P2-25: the headphone device for layout 3 (a device name of the main driver). Empty = the first other one.
inline const SettingKey<juce::String> cueDevice{"cueDevice", {}};

// The mixer as it was left (saved when the app closes, from the engine, so knobs and menus both count):
// headphone mix 0 = cue only (the default: the headphones prepare the next track) .. 1 = master only, headphone
// level dB, crossfader curve (0 smooth, 1 linear, 2 cut), each deck's crossfader side (0 A, 1 THRU, 2 B).
inline const SettingKey<double> headphoneMix{"headphoneMix", 0.0};
inline const SettingKey<double> headphoneLevelDb{"headphoneLevelDb", 0.0};
inline const SettingKey<int> crossfaderCurve{"crossfaderCurve", 0};
inline const SettingKey<int> crossfaderAssignA{"crossfaderAssignA", 0};
inline const SettingKey<int> crossfaderAssignB{"crossfaderAssignB", 2};

// Show the Debug menu (crash tests, log folder) in Release builds too. Debug builds always show it.
inline const SettingKey<bool> showDebugMenu{"showDebugMenu", false};

// Folder shown in the temporary file browser (P1-18). Empty = the Music folder.
inline const SettingKey<juce::String> browserFolder{"browserFolder", {}};

// Library track table columns (P2-04): order, widths, hidden columns and sort, as written by
// juce::TableHeaderComponent::toString(). Empty = the default layout.
inline const SettingKey<juce::String> libraryColumns{"libraryColumns", {}};

// How keys are shown (P2-15): 0 = musical ("F#m"), 1 = Camelot ("11A").
inline const SettingKey<int> keyNotation{"keyNotation", 0};

// Auto Gain (P2-16): a deck's trim is set when a track loads so it plays at the target loudness (LUFS).
inline const SettingKey<bool> autoGain{"autoGain", true};
inline const SettingKey<int> autoGainTarget{"autoGainTarget", -10};

// Quantize (P2-20): on / off and the resolution in eighths of a beat (1 = 1/8, 2 = 1/4, 4 = 1/2, 8 = 1 beat).
inline const SettingKey<bool> quantize{"quantize", false};
inline const SettingKey<int> quantizeEighths{"quantizeEighths", 8};

// Key Sync (P2-22): also accept keys compatible with the master's (a step round the Camelot wheel), not only its
// own Camelot number - smaller moves.
inline const SettingKey<bool> keySyncCompatible{"keySyncCompatible", false};
// P3-05: how PAUSE stops a playing deck (0 instantly, 1 vinyl brake, 2 spin-back) and the stop times in seconds.
inline const SettingKey<int> pauseStyle{"pauseStyle", 0};
inline const SettingKey<double> brakeSeconds{"brakeSeconds", 1.0};
inline const SettingKey<double> spinBackSeconds{"spinBackSeconds", 1.0};

} // namespace devildj::app::keys
