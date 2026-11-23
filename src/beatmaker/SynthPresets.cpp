#include "beatmaker/SynthPresets.h"

#include <juce_core/juce_core.h>

#include <array>

namespace devildj::beatmaker
{

namespace
{
using dsp::Adsr;
using dsp::SynthEngine;
using dsp::SynthParams;

constexpr std::array<const char*, 36> kNames = {
    "gain",      "glide",     "ampA",      "ampD",         "ampS",       "ampR",     "osc1Wave", "osc2Wave",
    "osc2Detune", "osc2Octave", "oscMix",   "sub",          "subWave",    "noise",    "filterMode", "filter24",
    "cutoff",    "resonance", "envAmount", "keyTrack",     "filterA",    "filterD",  "filterS",  "filterR",
    "unison",    "unisonDetune", "ratio",  "index",        "indexA",     "indexD",   "indexS",   "indexR",
    "feedback",  "harmonic2", "drive",     "click"};

float* field(SynthParams& p, int i)
{
    float* const all[] = {&p.gain,      &p.glide,      &p.amp.attack,      &p.amp.decay,       &p.amp.sustain,
                          &p.amp.release, &p.osc1Wave, &p.osc2Wave,        &p.osc2Detune,      &p.osc2Octave,
                          &p.oscMix,    &p.sub,        &p.subWave,         &p.noise,           &p.filterMode,
                          &p.filter24,  &p.cutoff,     &p.resonance,       &p.envAmount,       &p.keyTrack,
                          &p.filterEnv.attack, &p.filterEnv.decay, &p.filterEnv.sustain, &p.filterEnv.release,
                          &p.unison,    &p.unisonDetune, &p.ratio,         &p.index,           &p.indexEnv.attack,
                          &p.indexEnv.decay, &p.indexEnv.sustain, &p.indexEnv.release, &p.feedback, &p.harmonic2,
                          &p.drive,     &p.click};
    return i < static_cast<int>(std::size(all)) ? all[i] : nullptr;
}

const char* engineName(SynthEngine e)
{
    return e == SynthEngine::Fm ? "fm" : e == SynthEngine::SubBass ? "subBass" : "subtractive";
}

SynthParams base(SynthEngine e, float gain, Adsr amp)
{
    SynthParams p;
    p.engine = e;
    p.gain = gain;
    p.amp = amp;
    return p;
}

std::vector<FactorySynthPreset> makeSubtractive()
{
    std::vector<FactorySynthPreset> v;
    const auto add = [&](const char* name, auto&& tweak)
    {
        auto p = base(SynthEngine::Subtractive, 0.5f, {0.005f, 0.3f, 0.7f, 0.25f});
        tweak(p);
        v.push_back({name, p});
    };
    add("Init Saw", [](SynthParams&) {});
    add("Supersaw", [](SynthParams& p) { p.unison = 8; p.unisonDetune = 30; p.cutoff = 6000; p.envAmount = 0.1f; p.gain = 0.4f; });
    add("Acid Bass", [](SynthParams& p) {
        p.osc1Wave = 0; p.oscMix = 0; p.cutoff = 300; p.resonance = 0.8f; p.envAmount = 0.6f;
        p.filterEnv = {0.001f, 0.18f, 0.0f, 0.1f}; p.amp = {0.002f, 0.2f, 0.8f, 0.05f}; p.glide = 0.06f; p.filter24 = 0; });
    add("Square Lead", [](SynthParams& p) { p.osc1Wave = 1; p.osc2Wave = 1; p.osc2Detune = 10; p.cutoff = 3000; p.glide = 0.03f; });
    add("Soft Pad", [](SynthParams& p) {
        p.osc1Wave = 0; p.osc2Wave = 2; p.unison = 4; p.unisonDetune = 20; p.cutoff = 1500; p.envAmount = 0.2f;
        p.amp = {0.6f, 1.0f, 0.8f, 1.5f}; p.filterEnv = {0.8f, 1.5f, 0.5f, 1.5f}; p.gain = 0.4f; });
    add("Pluck", [](SynthParams& p) {
        p.cutoff = 600; p.envAmount = 0.7f; p.filterEnv = {0.001f, 0.25f, 0.0f, 0.2f}; p.amp = {0.001f, 0.5f, 0.0f, 0.2f}; });
    add("Reese Bass", [](SynthParams& p) {
        p.osc2Detune = 25; p.oscMix = 0.5f; p.sub = 0.5f; p.subWave = 1; p.cutoff = 800; p.filter24 = 1;
        p.envAmount = 0.0f; p.amp = {0.005f, 0.3f, 1.0f, 0.2f}; });
    add("Brass", [](SynthParams& p) {
        p.oscMix = 0.4f; p.osc2Octave = 1; p.cutoff = 900; p.envAmount = 0.5f; p.filterEnv = {0.06f, 0.4f, 0.4f, 0.3f};
        p.amp = {0.03f, 0.3f, 0.8f, 0.3f}; });
    add("Noise Sweep", [](SynthParams& p) {
        p.oscMix = 0; p.noise = 0.8f; p.filterMode = 1; p.cutoff = 400; p.resonance = 0.6f; p.envAmount = 0.8f;
        p.filterEnv = {1.0f, 2.0f, 0.3f, 1.0f}; p.amp = {0.5f, 1.0f, 0.7f, 1.0f}; p.gain = 0.4f; });
    add("Hollow Keys", [](SynthParams& p) {
        p.osc1Wave = 2; p.osc2Wave = 1; p.osc2Octave = 1; p.oscMix = 0.3f; p.filterMode = 2; p.cutoff = 200;
        p.amp = {0.003f, 0.8f, 0.3f, 0.4f}; });
    return v;
}

std::vector<FactorySynthPreset> makeFm()
{
    std::vector<FactorySynthPreset> v;
    const auto add = [&](const char* name, float ratio, float index, Adsr indexEnv, Adsr amp, float feedback = 0.0f)
    {
        auto p = base(SynthEngine::Fm, 0.5f, amp);
        p.ratio = ratio;
        p.index = index;
        p.indexEnv = indexEnv;
        p.feedback = feedback;
        v.push_back({name, p});
    };
    add("Init FM", 2.0f, 2.0f, {0.001f, 0.4f, 0.3f, 0.3f}, {0.005f, 0.3f, 0.7f, 0.3f});
    add("Bell", 3.5f, 4.0f, {0.001f, 2.0f, 0.0f, 1.0f}, {0.001f, 3.0f, 0.0f, 2.0f});
    add("Tubular", 2.76f, 3.0f, {0.001f, 1.5f, 0.1f, 1.0f}, {0.001f, 2.5f, 0.0f, 1.5f});
    add("Pluck", 1.0f, 5.0f, {0.001f, 0.15f, 0.0f, 0.1f}, {0.001f, 0.6f, 0.0f, 0.2f});
    add("FM Bass", 1.0f, 3.0f, {0.001f, 0.2f, 0.2f, 0.1f}, {0.002f, 0.3f, 0.8f, 0.08f}, 0.2f);
    add("Electric Piano", 1.0f, 1.5f, {0.001f, 0.8f, 0.1f, 0.4f}, {0.001f, 1.5f, 0.2f, 0.5f});
    add("Metallic", 1.41f, 6.0f, {0.001f, 0.5f, 0.2f, 0.3f}, {0.001f, 0.8f, 0.2f, 0.4f}, 0.4f);
    add("Growl", 0.5f, 4.0f, {0.05f, 0.3f, 0.6f, 0.2f}, {0.01f, 0.3f, 0.9f, 0.2f}, 0.6f);
    add("Glass", 7.0f, 1.2f, {0.001f, 1.0f, 0.0f, 0.8f}, {0.001f, 2.0f, 0.0f, 1.2f});
    add("Wobble Lead", 2.0f, 2.5f, {0.2f, 0.5f, 0.5f, 0.3f}, {0.01f, 0.3f, 0.8f, 0.3f}, 0.3f);
    return v;
}

std::vector<FactorySynthPreset> makeSubBass()
{
    std::vector<FactorySynthPreset> v;
    const auto add = [&](const char* name, float harmonic2, float drive, float click, float glide, Adsr amp)
    {
        auto p = base(SynthEngine::SubBass, 0.6f, amp);
        p.harmonic2 = harmonic2;
        p.drive = drive;
        p.click = click;
        p.glide = glide;
        v.push_back({name, p});
    };
    add("Pure Sub", 0.0f, 0.0f, 0.0f, 0.0f, {0.003f, 0.5f, 0.9f, 0.2f});
    add("808 Long", 0.2f, 0.2f, 0.3f, 0.0f, {0.001f, 2.5f, 0.0f, 0.8f});
    add("808 Glide", 0.2f, 0.25f, 0.3f, 0.12f, {0.001f, 2.5f, 0.0f, 0.8f});
    add("Distorted 808", 0.3f, 0.8f, 0.4f, 0.08f, {0.001f, 2.0f, 0.0f, 0.6f});
    add("Short Sub", 0.0f, 0.1f, 0.2f, 0.0f, {0.001f, 0.3f, 0.0f, 0.1f});
    add("Warm Sub", 0.5f, 0.3f, 0.0f, 0.0f, {0.01f, 0.5f, 0.8f, 0.3f});
    add("Punchy", 0.3f, 0.4f, 0.8f, 0.0f, {0.001f, 0.6f, 0.4f, 0.2f});
    add("Slide", 0.1f, 0.2f, 0.1f, 0.25f, {0.005f, 0.5f, 0.9f, 0.3f});
    add("Round", 0.15f, 0.05f, 0.0f, 0.03f, {0.02f, 0.4f, 0.85f, 0.4f});
    add("Gritty", 0.6f, 0.9f, 0.5f, 0.05f, {0.002f, 0.8f, 0.6f, 0.25f});
    return v;
}
} // namespace

const std::vector<FactorySynthPreset>& factorySynthPresets(dsp::SynthEngine engine)
{
    static const auto subtractive = makeSubtractive();
    static const auto fm = makeFm();
    static const auto sub = makeSubBass();
    return engine == SynthEngine::Fm ? fm : engine == SynthEngine::SubBass ? sub : subtractive;
}

std::span<const char* const> synthParamNames()
{
    return kNames;
}

dsp::SynthParams synthParamsOf(const SynthPreset& preset)
{
    SynthParams p;
    p.engine = preset.engine == SynthPreset::Engine::Fm        ? SynthEngine::Fm
               : preset.engine == SynthPreset::Engine::SubBass ? SynthEngine::SubBass
                                                               : SynthEngine::Subtractive;
    const auto names = synthParamNames();
    for (std::size_t i = 0; i < names.size(); ++i)
        if (const auto it = preset.params.find(names[i]); it != preset.params.end())
            *field(p, static_cast<int>(i)) = it->second;
    return p;
}

SynthPreset synthPresetOf(const std::string& name, const dsp::SynthParams& params)
{
    SynthPreset out;
    out.name = name;
    out.engine = params.engine == SynthEngine::Fm        ? SynthPreset::Engine::Fm
                 : params.engine == SynthEngine::SubBass ? SynthPreset::Engine::SubBass
                                                         : SynthPreset::Engine::Subtractive;
    auto p = params;
    const auto names = synthParamNames();
    for (std::size_t i = 0; i < names.size(); ++i)
        out.params[names[i]] = *field(p, static_cast<int>(i));
    return out;
}

std::filesystem::path userPresetFolder()
{
    const auto docs = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    return std::filesystem::path(docs.getFullPathName().toWideCharPointer()) / L"Devil-DJ" / L"Beats" / L"Presets";
}

util::Result<bool> saveSynthPreset(const SynthPreset& preset, const std::filesystem::path& file)
{
    auto* o = new juce::DynamicObject();
    const juce::var root(o);
    o->setProperty("format", "devil-dj synth preset");
    o->setProperty("version", 1);
    o->setProperty("name", juce::String::fromUTF8(preset.name.c_str()));
    o->setProperty("engine", engineName(synthParamsOf(preset).engine));
    auto* params = new juce::DynamicObject();
    for (const auto& [k, v] : preset.params)
        params->setProperty(juce::Identifier(juce::String::fromUTF8(k.c_str())), static_cast<double>(v));
    o->setProperty("params", juce::var(params));
    const juce::File f(juce::String(file.wstring().c_str()));
    if (!f.getParentDirectory().createDirectory() || !f.replaceWithText(juce::JSON::toString(root, false)))
        return util::Result<bool>::fail("Can't write " + f.getFullPathName().toStdString());
    return util::Result<bool>::ok(true);
}

util::Result<SynthPreset> loadSynthPreset(const std::filesystem::path& file)
{
    const juce::File f(juce::String(file.wstring().c_str()));
    juce::var root;
    if (!f.existsAsFile() || juce::JSON::parse(f.loadFileAsString(), root).failed() || root.getDynamicObject() == nullptr)
        return util::Result<SynthPreset>::fail("Not a synth preset: " + f.getFullPathName().toStdString());
    SynthPreset p;
    p.name = root.getProperty("name", "").toString().toStdString();
    const auto engine = root.getProperty("engine", "subtractive").toString();
    p.engine = engine == "fm" ? SynthPreset::Engine::Fm : engine == "subBass" ? SynthPreset::Engine::SubBass
                                                                           : SynthPreset::Engine::Subtractive;
    if (const auto* params = root.getProperty("params", {}).getDynamicObject())
        for (const auto& kv : params->getProperties())
            if (kv.value.isDouble() || kv.value.isInt() || kv.value.isInt64())
                p.params[kv.name.toString().toStdString()] = static_cast<float>(static_cast<double>(kv.value));
    return util::Result<SynthPreset>::ok(std::move(p));
}

} // namespace devildj::beatmaker
