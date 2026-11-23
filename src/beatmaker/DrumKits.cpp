#include "beatmaker/DrumKits.h"

#include <cstring>

namespace devildj::beatmaker
{

namespace
{
using dsp::DrumParams;

// DrumParams field order: tune, decay, tone, snap, drive, pitchAmount, click, spread.
constexpr DrumPreset kKick[] = {
    {"Default", {0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f}},
    {"Tight", {0.6f, 0.15f, 0.5f, 0.5f, 0.1f, 0.4f, 0.7f, 0.5f}},
    {"Boomy", {0.2f, 0.85f, 0.5f, 0.5f, 0.0f, 0.7f, 0.3f, 0.5f}},
    {"Distorted", {0.4f, 0.5f, 0.5f, 0.5f, 0.7f, 0.6f, 0.6f, 0.5f}},
    {"808", {0.15f, 1.0f, 0.5f, 0.5f, 0.15f, 0.3f, 0.1f, 0.5f}},
};
constexpr DrumPreset kSnare[] = {
    {"Default", {0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f}},
    {"Tight", {0.7f, 0.2f, 0.6f, 0.2f, 0.1f, 0.5f, 0.5f, 0.5f}},
    {"Fat", {0.25f, 0.8f, 0.35f, 0.6f, 0.2f, 0.5f, 0.5f, 0.5f}},
    {"Noisy", {0.5f, 0.4f, 0.9f, 0.8f, 0.0f, 0.5f, 0.5f, 0.5f}},
};
constexpr DrumPreset kClap[] = {
    {"Default", {0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f}},
    {"Tight", {0.5f, 0.15f, 0.6f, 0.5f, 0.0f, 0.5f, 0.5f, 0.2f}},
    {"Wide", {0.5f, 0.8f, 0.4f, 0.5f, 0.0f, 0.5f, 0.5f, 0.9f}},
    {"Crunchy", {0.5f, 0.5f, 0.7f, 0.5f, 0.6f, 0.5f, 0.5f, 0.5f}},
};
constexpr DrumPreset kClosedHat[] = {
    {"Default", {0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f}},
    {"Tick", {0.7f, 0.1f, 0.8f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f}},
    {"Dark", {0.3f, 0.5f, 0.15f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f}},
};
constexpr DrumPreset kOpenHat[] = {
    {"Default", {0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f}},
    {"Short", {0.5f, 0.15f, 0.6f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f}},
    {"Washy", {0.4f, 1.0f, 0.4f, 0.5f, 0.1f, 0.5f, 0.5f, 0.5f}},
};
constexpr DrumPreset kTom[] = {
    {"Default", {0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f}},
    {"Short", {0.5f, 0.15f, 0.5f, 0.5f, 0.0f, 0.3f, 0.5f, 0.5f}},
    {"Deep", {0.2f, 0.8f, 0.5f, 0.5f, 0.0f, 0.8f, 0.5f, 0.5f}},
};
constexpr DrumPreset kRim[] = {
    {"Default", {0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f}},
    {"Click", {0.8f, 0.1f, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f}},
    {"Wood", {0.2f, 0.7f, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f}},
};
constexpr DrumPreset kCowbell[] = {
    {"Default", {0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f}},
    {"Short", {0.5f, 0.1f, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f}},
    {"Low", {0.15f, 0.6f, 0.5f, 0.5f, 0.2f, 0.5f, 0.5f, 0.5f}},
};
constexpr DrumPreset kShaker[] = {
    {"Default", {0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f}},
    {"Tight", {0.7f, 0.1f, 0.7f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f}},
    {"Soft", {0.5f, 0.7f, 0.2f, 0.5f, 0.0f, 0.5f, 0.5f, 0.5f}},
};

constexpr Kit kKits[] = {
    {"Club",
     {{{"kick", "Default"}, {"snare", "Default"}, {"clap", "Default"}, {"closedHat", "Default"}, {"openHat", "Default"},
       {"tomLow", "Default"}, {"rim", "Default"}, {"shaker", "Default"}}}},
    {"Minimal",
     {{{"kick", "Tight"}, {"rim", "Click"}, {"clap", "Tight"}, {"closedHat", "Tick"}, {"openHat", "Short"},
       {"tomMid", "Short"}, {"cowbell", "Short"}, {"shaker", "Tight"}}}},
    {"Breaks",
     {{{"kick", "Boomy"}, {"snare", "Fat"}, {"clap", "Wide"}, {"closedHat", "Default"}, {"openHat", "Washy"},
       {"tomLow", "Deep"}, {"tomHigh", "Default"}, {"shaker", "Soft"}}}},
    {"Trap",
     {{{"kick", "808"}, {"snare", "Tight"}, {"clap", "Crunchy"}, {"closedHat", "Tick"}, {"openHat", "Short"},
       {"tomLow", "Deep"}, {"rim", "Click"}, {"cowbell", "Low"}}}},
    {"Lo-fi",
     {{{"kick", "Distorted"}, {"snare", "Noisy"}, {"clap", "Default"}, {"closedHat", "Dark"}, {"openHat", "Default"},
       {"tomMid", "Default"}, {"rim", "Wood"}, {"shaker", "Soft"}}}},
};

float param(const SoundRef& s, const char* name, float fallback)
{
    const auto it = s.params.find(name);
    return it != s.params.end() ? it->second : fallback;
}
} // namespace

std::span<const DrumPreset> drumPresets(dsp::DrumKind kind)
{
    switch (kind)
    {
        case dsp::DrumKind::Kick: return kKick;
        case dsp::DrumKind::Snare: return kSnare;
        case dsp::DrumKind::Clap: return kClap;
        case dsp::DrumKind::ClosedHat: return kClosedHat;
        case dsp::DrumKind::OpenHat: return kOpenHat;
        case dsp::DrumKind::TomLow:
        case dsp::DrumKind::TomMid:
        case dsp::DrumKind::TomHigh: return kTom;
        case dsp::DrumKind::Rim: return kRim;
        case dsp::DrumKind::Cowbell: return kCowbell;
        case dsp::DrumKind::Shaker: return kShaker;
    }
    return kKick;
}

const DrumPreset* findDrumPreset(dsp::DrumKind kind, const std::string& name)
{
    for (const auto& p : drumPresets(kind))
        if (name == p.name)
            return &p;
    return nullptr;
}

std::span<const Kit> drumKits()
{
    return kKits;
}

dsp::DrumParams drumParamsOf(const SoundRef& s)
{
    dsp::DrumParams p;
    p.tune = param(s, "tune", p.tune);
    p.decay = param(s, "decay", p.decay);
    p.tone = param(s, "tone", p.tone);
    p.snap = param(s, "snap", p.snap);
    p.drive = param(s, "drive", p.drive);
    p.pitchAmount = param(s, "pitch", p.pitchAmount);
    p.click = param(s, "click", p.click);
    p.spread = param(s, "spread", p.spread);
    return p;
}

SoundRef drumSoundFromPreset(dsp::DrumKind kind, const DrumPreset& preset)
{
    SoundRef s;
    s.kind = SoundRef::Kind::Synth;
    s.synth = dsp::drumKindName(kind);
    const auto& p = preset.params;
    s.params = {{"tune", p.tune},   {"decay", p.decay},        {"tone", p.tone},   {"snap", p.snap},
                {"drive", p.drive}, {"pitch", p.pitchAmount}, {"click", p.click}, {"spread", p.spread}};
    return s;
}

std::vector<DrumTrack> kitTracks(const Kit& kit, int lengthSteps)
{
    std::vector<DrumTrack> out;
    for (const auto& k : kit.sounds)
    {
        auto t = drumTrack(k.synth, lengthSteps);
        const auto kind = dsp::drumKindFromName(k.synth);
        if (const auto* p = findDrumPreset(kind, k.preset))
            t.sound = drumSoundFromPreset(kind, *p);
        out.push_back(std::move(t));
    }
    return out;
}

engine::DrumSound engineDrumSound(const SoundRef& sound)
{
    return {dsp::drumKindFromName(sound.synth.c_str()), drumParamsOf(sound)};
}

} // namespace devildj::beatmaker
