#pragma once

// Transition presets (P4-15): our own set, each a set of lanes on a 0..1 timeline (start .. end of the transition)
// stretched to the length chosen (1-32 bars), and user presets saved as JSON in Documents\Devil-DJ\Transitions -
// also on the 0..1 timeline, so one saved preset works at any length.
//
//   Smooth blend   both volumes cross with S-curves, the basses swap in the middle
//   Bass swap      the new track comes in early without its lows; at the middle the basses swap; the old one fades
//                  out at the end
//   Filter rise    the new track rises out of a high-pass while the old one drains into one
//   High-pass out  the old track sweeps up through the high-pass and away; the new one arrives without its lows
//                  until three quarters in
//   Echo out       the old track plays on, then ends on its echo (the FX lane, P4-18) as the new one starts
//   Quick cut      a hard switch on the middle beat
//   Long blend     a long, gentle cross: mids dipped on both while they overlap, basses swapped at the middle
//   Loop out       the old track's last bar loops (P4-18) and fades while the new one comes in

#include "engine/TransitionCurves.h"

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

namespace devildj::app
{

enum class TransitionPreset
{
    SmoothBlend,
    BassSwap,
    FilterRise,
    HighPassOut,
    EchoOut,
    QuickCut,
    LongBlend,
    LoopOut,
};
constexpr int kTransitionPresetCount = 8;

const char* presetName(TransitionPreset preset) noexcept;
int presetDefaultBars(TransitionPreset preset) noexcept;
// The preset at `beats` long (curves normalized).
engine::TransitionCurves makePreset(TransitionPreset preset, float beats);
// The same, on the 0..1 timeline (beats = 1): what user presets store.
engine::TransitionCurves presetShape(TransitionPreset preset);

// A shape (0..1 timeline) stretched to `beats`; and a transition squeezed back onto 0..1.
engine::TransitionCurves scaled(const engine::TransitionCurves& shape, float beats);
engine::TransitionCurves unitShape(const engine::TransitionCurves& curves);

// JSON (the 0..1 shape and its name).
juce::String curvesToJson(const engine::TransitionCurves& shape, const juce::String& name);
bool curvesFromJson(const juce::String& json, engine::TransitionCurves& shape, juce::String& name);

// User presets on disk: Documents\Devil-DJ\Transitions\<name>.json.
juce::File userTransitionFolder();
juce::String saveUserPreset(const engine::TransitionCurves& curves, const juce::String& name,
                            const juce::File& folder = userTransitionFolder()); // error or empty
struct UserPreset
{
    juce::String name;
    engine::TransitionCurves shape; // 0..1
};
std::vector<UserPreset> loadUserPresets(const juce::File& folder = userTransitionFolder());

} // namespace devildj::app
