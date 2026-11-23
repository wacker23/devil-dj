#include "app/TransitionPresets.h"

#include "dsp/fx/BeatFx.h"

#include <algorithm>

namespace devildj::app
{

using engine::Lane;
using engine::Shape;
using engine::TransitionCurves;

namespace
{
constexpr float kKill = -1000.0f;

void set(TransitionCurves& c, bool incoming, Lane lane, std::vector<engine::CurvePoint> points)
{
    c.lane(incoming, lane).points = std::move(points);
}

// The basses swapped at `at`: the old one's lows go, the new one's come.
void bassSwap(TransitionCurves& c, float at)
{
    set(c, false, Lane::EqLow, {{0.0f, 0.0f, Shape::Hold}, {at, kKill}});
    set(c, true, Lane::EqLow, {{0.0f, kKill, Shape::Hold}, {at, 0.0f}});
}

const char* shapeName(Shape s)
{
    switch (s)
    {
        case Shape::EaseIn: return "easeIn";
        case Shape::EaseOut: return "easeOut";
        case Shape::SCurve: return "s";
        case Shape::Hold: return "hold";
        case Shape::Linear: break;
    }
    return "linear";
}

Shape shapeFrom(const juce::String& s)
{
    return s == "easeIn" ? Shape::EaseIn : s == "easeOut" ? Shape::EaseOut : s == "s" ? Shape::SCurve : s == "hold" ? Shape::Hold : Shape::Linear;
}

const char* laneName(int lane)
{
    constexpr const char* names[] = {"volume", "eqLow", "eqMid", "eqHigh", "filter", "fx"};
    return names[lane];
}
} // namespace

const char* presetName(TransitionPreset p) noexcept
{
    switch (p)
    {
        case TransitionPreset::SmoothBlend: return "Smooth blend";
        case TransitionPreset::BassSwap: return "Bass swap";
        case TransitionPreset::FilterRise: return "Filter rise";
        case TransitionPreset::HighPassOut: return "High-pass out";
        case TransitionPreset::EchoOut: return "Echo out";
        case TransitionPreset::QuickCut: return "Quick cut";
        case TransitionPreset::LongBlend: return "Long blend";
        case TransitionPreset::LoopOut: return "Loop out";
    }
    return "";
}

int presetDefaultBars(TransitionPreset p) noexcept
{
    switch (p)
    {
        case TransitionPreset::QuickCut: return 1;
        case TransitionPreset::EchoOut: return 4;
        case TransitionPreset::LoopOut: return 4;
        case TransitionPreset::LongBlend: return 32;
        case TransitionPreset::SmoothBlend:
        case TransitionPreset::BassSwap:
        case TransitionPreset::FilterRise:
        case TransitionPreset::HighPassOut: return 8;
    }
    return 8;
}

TransitionCurves presetShape(TransitionPreset p)
{
    TransitionCurves c;
    c.beats = 1.0f;
    switch (p)
    {
        case TransitionPreset::SmoothBlend:
            set(c, false, Lane::Volume, {{0.0f, 1.0f}, {0.5f, 1.0f, Shape::SCurve}, {1.0f, 0.0f}});
            set(c, true, Lane::Volume, {{0.0f, 0.0f, Shape::SCurve}, {0.5f, 1.0f}});
            bassSwap(c, 0.5f);
            break;
        case TransitionPreset::BassSwap:
            set(c, true, Lane::Volume, {{0.0f, 0.0f, Shape::EaseOut}, {0.25f, 1.0f}});
            set(c, true, Lane::EqHigh, {{0.0f, -12.0f, Shape::EaseOut}, {0.25f, 0.0f}});
            set(c, false, Lane::Volume, {{0.75f, 1.0f, Shape::EaseIn}, {1.0f, 0.0f}});
            bassSwap(c, 0.5f);
            break;
        case TransitionPreset::FilterRise:
            set(c, true, Lane::Filter, {{0.0f, 0.8f, Shape::EaseIn}, {0.75f, 0.0f}});
            set(c, true, Lane::Volume, {{0.0f, 0.0f, Shape::EaseOut}, {0.25f, 1.0f}});
            set(c, false, Lane::Filter, {{0.5f, 0.0f, Shape::EaseIn}, {1.0f, 0.9f}});
            set(c, false, Lane::Volume, {{0.75f, 1.0f}, {1.0f, 0.0f}});
            bassSwap(c, 0.75f);
            break;
        case TransitionPreset::HighPassOut:
            set(c, false, Lane::Filter, {{0.0f, 0.0f, Shape::EaseIn}, {1.0f, 1.0f}});
            set(c, false, Lane::Volume, {{0.8f, 1.0f}, {1.0f, 0.0f}});
            set(c, true, Lane::Volume, {{0.0f, 0.0f, Shape::SCurve}, {0.5f, 1.0f}});
            set(c, true, Lane::EqLow, {{0.0f, kKill, Shape::Hold}, {0.75f, 0.0f}});
            break;
        case TransitionPreset::EchoOut:
            c.fx = {static_cast<std::int8_t>(dsp::fx::EffectType::Echo), -1}; // a 3/4-beat echo on the old track
            c.fxBeats = 0.75f;
            set(c, false, Lane::FxSend, {{0.5f, 0.0f, Shape::EaseIn}, {0.75f, 1.0f}});
            set(c, false, Lane::Volume, {{0.75f, 1.0f, Shape::Hold}, {0.76f, 0.0f}});
            set(c, true, Lane::Volume, {{0.75f, 0.0f, Shape::EaseOut}, {0.8f, 1.0f}});
            break;
        case TransitionPreset::QuickCut:
            set(c, false, Lane::Volume, {{0.0f, 1.0f, Shape::Hold}, {0.5f, 0.0f}});
            set(c, true, Lane::Volume, {{0.0f, 0.0f, Shape::Hold}, {0.5f, 1.0f}});
            break;
        case TransitionPreset::LongBlend:
            set(c, false, Lane::Volume, {{0.0f, 1.0f}, {0.6f, 1.0f, Shape::SCurve}, {1.0f, 0.0f}});
            set(c, true, Lane::Volume, {{0.0f, 0.0f, Shape::SCurve}, {0.4f, 1.0f}});
            set(c, false, Lane::EqMid, {{0.2f, 0.0f, Shape::SCurve}, {0.5f, -6.0f}});
            set(c, true, Lane::EqMid, {{0.2f, -6.0f, Shape::SCurve}, {0.8f, 0.0f}});
            bassSwap(c, 0.5f);
            break;
        case TransitionPreset::LoopOut:
            c.loopBeats = 4.0f; // one bar, whatever the length (not scaled)
            set(c, false, Lane::Volume, {{0.0f, 1.0f}, {1.0f, 0.0f}});
            set(c, true, Lane::Volume, {{0.0f, 0.0f, Shape::EaseOut}, {0.5f, 1.0f}});
            bassSwap(c, 0.5f);
            break;
    }
    return c;
}

TransitionCurves scaled(const TransitionCurves& shape, float beats)
{
    TransitionCurves c = shape;
    c.beats = std::clamp(beats, 1.0f, 256.0f);
    const float k = c.beats / std::max(1e-6f, shape.beats);
    for (auto* side : {&c.from, &c.to})
        for (auto& lane : *side)
            for (auto& pt : lane.points)
                pt.beat *= k;
    engine::normalize(c);
    return c;
}

TransitionCurves unitShape(const TransitionCurves& curves)
{
    TransitionCurves c = curves;
    const float k = 1.0f / std::max(1e-6f, curves.beats);
    for (auto* side : {&c.from, &c.to})
        for (auto& lane : *side)
            for (auto& pt : lane.points)
                pt.beat *= k;
    c.beats = 1.0f;
    return c;
}

TransitionCurves makePreset(TransitionPreset preset, float beats)
{
    return scaled(presetShape(preset), beats);
}

juce::String curvesToJson(const TransitionCurves& shapeIn, const juce::String& name)
{
    const auto shape = shapeIn.beats == 1.0f ? shapeIn : unitShape(shapeIn);
    auto* root = new juce::DynamicObject();
    const juce::var keep(root);
    root->setProperty("format", "devil-dj transition");
    root->setProperty("version", 1);
    root->setProperty("name", name);
    root->setProperty("loopBeats", static_cast<double>(shape.loopBeats));
    root->setProperty("fxOut", static_cast<int>(shape.fx[0]));
    root->setProperty("fxIn", static_cast<int>(shape.fx[1]));
    root->setProperty("fxBeats", static_cast<double>(shape.fxBeats));
    for (int k = 0; k < 2; ++k)
    {
        auto* side = new juce::DynamicObject();
        for (int l = 0; l < engine::kLaneCount; ++l)
        {
            const auto& lane = (k == 0 ? shape.from : shape.to)[static_cast<std::size_t>(l)];
            if (!lane.used())
                continue;
            juce::Array<juce::var> pts;
            for (const auto& p : lane.points)
                pts.add(juce::Array<juce::var>{static_cast<double>(p.beat), static_cast<double>(p.value), shapeName(p.shape)});
            side->setProperty(laneName(l), pts);
        }
        root->setProperty(k == 0 ? "out" : "in", juce::var(side));
    }
    return juce::JSON::toString(keep, false);
}

bool curvesFromJson(const juce::String& json, TransitionCurves& shape, juce::String& name)
{
    juce::var root;
    if (juce::JSON::parse(json, root).failed() || root.getDynamicObject() == nullptr ||
        root.getProperty("format", {}).toString() != "devil-dj transition")
        return false;
    shape = {};
    shape.beats = 1.0f;
    name = root.getProperty("name", "").toString();
    shape.loopBeats = static_cast<float>(static_cast<double>(root.getProperty("loopBeats", 0.0)));
    shape.fx = {static_cast<std::int8_t>(static_cast<int>(root.getProperty("fxOut", -1))),
                static_cast<std::int8_t>(static_cast<int>(root.getProperty("fxIn", -1)))};
    shape.fxBeats = static_cast<float>(static_cast<double>(root.getProperty("fxBeats", 0.75)));
    for (int k = 0; k < 2; ++k)
    {
        const auto side = root.getProperty(k == 0 ? "out" : "in", {});
        for (int l = 0; l < engine::kLaneCount; ++l)
            if (const auto* pts = side.getProperty(laneName(l), {}).getArray())
                for (const auto& p : *pts)
                    if (p.isArray() && p.size() >= 2)
                        (k == 0 ? shape.from : shape.to)[static_cast<std::size_t>(l)].points.push_back(
                            {static_cast<float>(static_cast<double>(p[0])), static_cast<float>(static_cast<double>(p[1])),
                             p.size() > 2 ? shapeFrom(p[2].toString()) : Shape::Linear});
    }
    engine::normalize(shape);
    return true;
}

juce::File userTransitionFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Devil-DJ").getChildFile("Transitions");
}

juce::String saveUserPreset(const TransitionCurves& curves, const juce::String& name, const juce::File& folder)
{
    const auto clean = juce::File::createLegalFileName(name.trim());
    if (clean.isEmpty())
        return "a preset needs a name";
    if (!folder.createDirectory())
        return "can't make " + folder.getFullPathName();
    const auto file = folder.getChildFile(clean + ".json");
    if (!file.replaceWithText(curvesToJson(curves, name.trim())))
        return "can't write " + file.getFullPathName();
    return {};
}

std::vector<UserPreset> loadUserPresets(const juce::File& folder)
{
    std::vector<UserPreset> out;
    for (const auto& f : folder.findChildFiles(juce::File::findFiles, false, "*.json"))
    {
        UserPreset p;
        if (curvesFromJson(f.loadFileAsString(), p.shape, p.name))
        {
            if (p.name.isEmpty())
                p.name = f.getFileNameWithoutExtension();
            out.push_back(std::move(p));
        }
    }
    std::sort(out.begin(), out.end(), [](const UserPreset& a, const UserPreset& b) { return a.name.compareIgnoreCase(b.name) < 0; });
    return out;
}

} // namespace devildj::app
