#pragma once

// Synth presets (BEATMAKER_SPEC §5): 10 factory presets per engine, the model's SynthPreset (engine + named
// numbers) <-> the DSP's SynthParams, and user presets as JSON files in Documents\Devil-DJ\Beats\Presets.

#include "beatmaker/Model.h"
#include "dsp/Synth.h"
#include "util/Result.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace devildj::beatmaker
{

struct FactorySynthPreset
{
    std::string name;
    dsp::SynthParams params;
};

const std::vector<FactorySynthPreset>& factorySynthPresets(dsp::SynthEngine engine);

// The parameter names a SynthPreset's `params` use (every field of SynthParams but the engine).
std::span<const char* const> synthParamNames();

dsp::SynthParams synthParamsOf(const SynthPreset& preset); // missing names keep the engine's defaults
SynthPreset synthPresetOf(const std::string& name, const dsp::SynthParams& params);

std::filesystem::path userPresetFolder(); // Documents\Devil-DJ\Beats\Presets
util::Result<bool> saveSynthPreset(const SynthPreset& preset, const std::filesystem::path& file);
util::Result<SynthPreset> loadSynthPreset(const std::filesystem::path& file);

} // namespace devildj::beatmaker
