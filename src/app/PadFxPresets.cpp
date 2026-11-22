#include "PadFxPresets.h"

#include <algorithm>

namespace devildj::app
{

namespace
{
juce::String beatsLabel(double beats)
{
    if (beats < 1.0)
        return "1/" + juce::String(juce::roundToInt(1.0 / beats));
    return juce::String(juce::roundToInt(beats));
}
} // namespace

PadFxPreset presetFor(bool release, int type)
{
    PadFxPreset p;
    p.release = release;
    p.type = std::clamp(type, 0, (release ? dsp::fx::ReleaseFx::kKindCount : dsp::fx::kEffectTypeCount) - 1);
    if (!release)
        if (auto fx = dsp::fx::makeEffect(static_cast<dsp::fx::EffectType>(p.type)))
            for (int i = 0; i < std::min(3, fx->numParams()); ++i)
                p.params[static_cast<std::size_t>(i)] = fx->param(i).defaultValue;
    return p;
}

PadFxPresets defaultPadFxPresets()
{
    using T = dsp::fx::EffectType;
    using K = dsp::fx::ReleaseFx::Kind;
    const auto beat = [](T type, float beats, float wet = 0.6f)
    {
        auto p = presetFor(false, static_cast<int>(type));
        p.beats = beats;
        p.wet = wet;
        return p;
    };
    const auto release = [](K kind, float beats)
    {
        auto p = presetFor(true, static_cast<int>(kind));
        p.beats = beats;
        p.wet = 1.0f;
        return p;
    };
    PadFxPresets all;
    all[0] = {beat(T::Echo, 0.5f), beat(T::Echo, 1.0f), beat(T::Reverb, 1.0f), beat(T::Flanger, 4.0f),
              beat(T::FilterLfo, 2.0f, 1.0f), beat(T::Roll, 0.25f, 1.0f), beat(T::Trans, 0.5f, 1.0f),
              release(K::EchoOut, 1.0f)};
    auto pitch = beat(T::Pitch, 1.0f, 1.0f);
    pitch.params[0] = (7.0f + 12.0f) / 24.0f; // +7 semitones
    all[1] = {beat(T::PingPong, 0.75f), beat(T::Spiral, 1.0f), beat(T::Phaser, 4.0f), beat(T::Noise, 4.0f), pitch,
              beat(T::Freeze, 0.5f, 1.0f), release(K::Brake, 2.0f), release(K::SpinBack, 1.0f)};
    return all;
}

juce::String presetName(const PadFxPreset& preset)
{
    if (preset.release)
        return dsp::fx::ReleaseFx::kindName(static_cast<dsp::fx::ReleaseFx::Kind>(preset.type));
    const auto type = static_cast<dsp::fx::EffectType>(preset.type);
    juce::String name = dsp::fx::effectTypeName(type);
    if (type == dsp::fx::EffectType::Pitch)
    {
        const int st = juce::roundToInt(-12.0f + 24.0f * preset.params[0]);
        return name + " " + (st > 0 ? "+" : "") + juce::String(st);
    }
    if (type == dsp::fx::EffectType::Reverb || type == dsp::fx::EffectType::BitCrush)
        return name;
    return name + " " + beatsLabel(preset.beats);
}

std::uint32_t presetColour(const PadFxPreset& preset)
{
    return preset.release ? 0xE08A3Cu : 0x9A5CE0u;
}

juce::String toJson(const PadFxPresets& presets)
{
    juce::Array<juce::var> banks;
    for (const auto& bank : presets)
    {
        juce::Array<juce::var> pads;
        for (const auto& p : bank)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("release", p.release);
            o->setProperty("type", p.type);
            o->setProperty("beats", p.beats);
            o->setProperty("wet", p.wet);
            o->setProperty("latch", p.latch);
            juce::Array<juce::var> params;
            for (const float v : p.params)
                params.add(v);
            o->setProperty("params", params);
            pads.add(juce::var(o));
        }
        banks.add(pads);
    }
    return juce::JSON::toString(juce::var(banks), true);
}

PadFxPresets presetsFromJson(const juce::String& json)
{
    auto presets = defaultPadFxPresets();
    const auto parsed = juce::JSON::parse(json);
    const auto* banks = parsed.getArray();
    if (banks == nullptr)
        return presets;
    for (int b = 0; b < std::min(kPadFxBanks, banks->size()); ++b)
    {
        const auto* pads = (*banks)[b].getArray();
        if (pads == nullptr)
            continue;
        for (int i = 0; i < std::min(kPadFxPads, pads->size()); ++i)
        {
            const auto& o = (*pads)[i];
            if (!o.isObject())
                continue;
            const bool release = static_cast<bool>(o.getProperty("release", false));
            const int type = static_cast<int>(o.getProperty("type", -1));
            const int count = release ? dsp::fx::ReleaseFx::kKindCount : dsp::fx::kEffectTypeCount;
            if (type < 0 || type >= count)
                continue;
            PadFxPreset p;
            p.release = release;
            p.type = type;
            p.beats = std::clamp(static_cast<float>(static_cast<double>(o.getProperty("beats", 1.0))), 1.0f / 16.0f, 16.0f);
            p.wet = std::clamp(static_cast<float>(static_cast<double>(o.getProperty("wet", 0.6))), 0.0f, 1.0f);
            p.latch = !release && static_cast<bool>(o.getProperty("latch", false));
            if (const auto* params = o.getProperty("params", {}).getArray())
                for (int k = 0; k < std::min(3, params->size()); ++k)
                    p.params[static_cast<std::size_t>(k)] =
                        std::clamp(static_cast<float>(static_cast<double>((*params)[k])), 0.0f, 1.0f);
            presets[static_cast<std::size_t>(b)][static_cast<std::size_t>(i)] = p;
        }
    }
    return presets;
}

std::vector<engine::EngineCommand> padFxCommands(const PadFxPreset& preset, engine::DeckId deck, int pad,
                                                 bool release, const engine::DeckState& state)
{
    std::vector<engine::EngineCommand> out;
    if (pad < 0 || pad >= kPadFxPads)
        return out;
    if (preset.release)
    {
        out.push_back(engine::ReleaseFxCommand{deck, static_cast<std::uint8_t>(preset.type), !release, preset.beats});
        return out;
    }
    const auto p = static_cast<std::uint8_t>(pad);
    if (preset.latch)
    {
        if (!release)
        {
            const bool on = (state.padFxOn & (1u << pad)) != 0;
            out.push_back(engine::PadFxCommand{deck, p, engine::FxOp::On, 0, on ? 0.0f : 1.0f});
        }
        return out;
    }
    out.push_back(engine::PadFxCommand{deck, p, engine::FxOp::On, 0, release ? 0.0f : 1.0f});
    return out;
}

} // namespace devildj::app
