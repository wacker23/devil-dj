#pragma once

// Drum-synth presets (3-5 per sound) and kits (named sets of 8 track sounds) - BEATMAKER_SPEC §4; and the bridge
// from the model's SoundRef (a synth name + named parameters) to the engine's drum sound.

#include "beatmaker/Model.h"
#include "dsp/DrumSynth.h"
#include "engine/Voices.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace devildj::beatmaker
{

struct DrumPreset
{
    const char* name;
    dsp::DrumParams params;
};

struct KitSound
{
    const char* synth;  // a model synth name ("kick", ...)
    const char* preset; // one of its presets
};

struct Kit
{
    const char* name;
    std::array<KitSound, 8> sounds;
};

std::span<const DrumPreset> drumPresets(dsp::DrumKind kind);
const DrumPreset* findDrumPreset(dsp::DrumKind kind, const std::string& name);
std::span<const Kit> drumKits(); // Club, Minimal, Breaks, Trap, Lo-fi

// The model's parameter names: tune, decay, tone, snap, drive, pitch, click, spread (0..1; missing = default).
dsp::DrumParams drumParamsOf(const SoundRef& sound);
SoundRef drumSoundFromPreset(dsp::DrumKind kind, const DrumPreset& preset);
// The kit's 8 drum tracks (named after the sounds, the hats in one choke group).
std::vector<DrumTrack> kitTracks(const Kit& kit, int lengthSteps = 16);
engine::DrumSound engineDrumSound(const SoundRef& sound);

} // namespace devildj::beatmaker
