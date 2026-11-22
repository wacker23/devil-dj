#pragma once

#include "dsp/fx/BeatFx.h"
#include "dsp/fx/ReleaseFx.h"
#include "engine/EngineCommand.h"
#include "engine/EngineState.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <vector>

namespace devildj::app
{

// Pad FX presets (P3-12): 2 banks x 8 pads, each an effect with its settings - a beat effect (P3-09) or a
// release effect (P3-11) - played from the pads in Pad FX mode (Shift+F3). Momentary: on while the pad is held;
// latched: a press turns it on, the next press off. Release effects are always held (let go = the release).
struct PadFxPreset
{
    bool release = false; // a release effect (kind) rather than a beat effect (type)
    int type = 0;         // dsp::fx::EffectType or dsp::fx::ReleaseFx::Kind
    float beats = 1.0f;
    float wet = 0.6f;
    std::array<float, 3> params{0.5f, 0.5f, 0.5f}; // the effect's knobs (normalised)
    bool latch = false;
};

constexpr int kPadFxBanks = 2;
constexpr int kPadFxPads = 8;
using PadFxBank = std::array<PadFxPreset, kPadFxPads>;
using PadFxPresets = std::array<PadFxBank, kPadFxBanks>;

// The ones to start with: bank 1 Echo 1/2, Echo 1, Reverb, Flanger, Filter LFO, Roll 1/4, Trans 1/2, Echo Out;
// bank 2 Ping Pong, Spiral, Phaser, Noise, Pitch +7, Freeze, Brake, Spin Back. Knobs at each effect's defaults.
PadFxPresets defaultPadFxPresets();
// The preset with an effect's default knobs (for a new choice of effect).
PadFxPreset presetFor(bool release, int type);

juce::String presetName(const PadFxPreset& preset); // "Echo 1/2", "Spin Back"
std::uint32_t presetColour(const PadFxPreset& preset);

// Saved as JSON in the settings; anything missing or out of range falls back to the defaults.
juce::String toJson(const PadFxPresets& presets);
PadFxPresets presetsFromJson(const juce::String& json);

// What a Pad FX pad press / release does - the keys and the mouse both send these. A beat effect switches the
// pad's slot on / off (latched: a press toggles, the release does nothing); a release effect is held (press) and
// let go (release) on the deck's release FX.
std::vector<engine::EngineCommand> padFxCommands(const PadFxPreset& preset, engine::DeckId deck, int pad,
                                                 bool release, const engine::DeckState& state);

} // namespace devildj::app
