// P4-15: transition presets. Done when: each preset's curves tested; a user preset can be saved and reloaded.

#include "app/TransitionPresets.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>

using namespace devildj;
using app::TransitionPreset;
using engine::evaluate;
using engine::Lane;

namespace
{
float at(const engine::TransitionCurves& c, bool incoming, Lane lane, double beat)
{
    return evaluate(c.lane(incoming, lane), beat);
}
} // namespace

TEST_CASE("Transition presets: every one hands over - the old track gone, the new one whole", "[app][transition][presets]")
{
    for (int i = 0; i < app::kTransitionPresetCount; ++i)
    {
        const auto p = static_cast<TransitionPreset>(i);
        const auto c = app::makePreset(p, 16.0f);
        INFO(app::presetName(p));
        CHECK(c.beats == 16.0f);
        CHECK(app::presetDefaultBars(p) >= 1);
        CHECK(app::presetDefaultBars(p) <= 32);
        for (const auto* side : {&c.from, &c.to})
            for (const auto& lane : *side)
                for (std::size_t k = 0; k < lane.points.size(); ++k)
                {
                    CHECK(lane.points[k].beat >= 0.0f);
                    CHECK(lane.points[k].beat <= 16.0f);
                    if (k > 0)
                        CHECK(lane.points[k - 1].beat <= lane.points[k].beat);
                }
        // At the end: the outgoing deck silent, the incoming one at full level with its EQ and filter neutral.
        REQUIRE(c.lane(false, Lane::Volume).used());
        CHECK(at(c, false, Lane::Volume, 16.0) == 0.0f);
        if (c.lane(true, Lane::Volume).used())
            CHECK(at(c, true, Lane::Volume, 16.0) == 1.0f);
        for (const auto lane : {Lane::EqLow, Lane::EqMid, Lane::EqHigh, Lane::Filter})
            if (c.lane(true, lane).used())
                CHECK(at(c, true, lane, 16.0) == 0.0f);
        // At the start the outgoing deck plays as it was (no lane drops it straight away).
        CHECK(at(c, false, Lane::Volume, 0.0) == 1.0f);
        if (c.lane(true, Lane::Volume).used() && p != TransitionPreset::QuickCut)
            CHECK(at(c, true, Lane::Volume, 0.0) == 0.0f);
    }
}

TEST_CASE("Transition presets: each one's shape", "[app][transition][presets]")
{
    using P = TransitionPreset;
    auto c = app::makePreset(P::SmoothBlend, 16.0f);
    CHECK(at(c, false, Lane::EqLow, 7.9) == 0.0f); // the basses swap on beat 8
    CHECK(at(c, false, Lane::EqLow, 8.0) <= -100.0f);
    CHECK(at(c, true, Lane::EqLow, 7.9) <= -100.0f);
    CHECK(at(c, true, Lane::EqLow, 8.0) == 0.0f);
    CHECK(at(c, true, Lane::Volume, 8.0) == 1.0f);
    CHECK(std::abs(at(c, true, Lane::Volume, 4.0) - 0.5f) < 1e-6f); // S-curve: half way at its middle

    c = app::makePreset(P::BassSwap, 32.0f);
    CHECK(at(c, true, Lane::Volume, 8.0) == 1.0f); // in early
    CHECK(at(c, true, Lane::EqHigh, 0.0) == -12.0f);
    CHECK(at(c, false, Lane::Volume, 24.0) == 1.0f); // the old one full until three quarters
    CHECK(at(c, false, Lane::EqLow, 16.0) <= -100.0f);

    c = app::makePreset(P::FilterRise, 16.0f);
    CHECK(at(c, true, Lane::Filter, 0.0) > 0.5f); // the new one through a high-pass ...
    CHECK(at(c, true, Lane::Filter, 6.0) < at(c, true, Lane::Filter, 2.0)); // ... opening
    CHECK(at(c, false, Lane::Filter, 16.0) > 0.5f); // the old one drains into one

    c = app::makePreset(P::HighPassOut, 16.0f);
    CHECK(at(c, false, Lane::Filter, 16.0) == 1.0f);
    CHECK(at(c, false, Lane::Filter, 4.0) < 0.25f); // ease in: slow at first
    CHECK(at(c, true, Lane::EqLow, 11.0) <= -100.0f);
    CHECK(at(c, true, Lane::EqLow, 12.0) == 0.0f);

    c = app::makePreset(P::EchoOut, 16.0f);
    CHECK(at(c, false, Lane::FxSend, 12.0) == 1.0f);
    CHECK(at(c, false, Lane::Volume, 11.9) == 1.0f);
    CHECK(at(c, false, Lane::Volume, 12.2) == 0.0f);

    c = app::makePreset(P::QuickCut, 4.0f);
    CHECK(at(c, false, Lane::Volume, 1.99) == 1.0f);
    CHECK(at(c, false, Lane::Volume, 2.0) == 0.0f);
    CHECK(at(c, true, Lane::Volume, 1.99) == 0.0f);
    CHECK(at(c, true, Lane::Volume, 2.0) == 1.0f);

    c = app::makePreset(P::LongBlend, 128.0f);
    CHECK(at(c, false, Lane::EqMid, 64.0) == -6.0f);
    CHECK(at(c, true, Lane::EqMid, 40.0) < 0.0f);

    CHECK(app::makePreset(P::LoopOut, 16.0f).loopBeats == 4.0f);
    CHECK(app::makePreset(P::LoopOut, 64.0f).loopBeats == 4.0f); // a bar, whatever the length
    CHECK(app::makePreset(P::SmoothBlend, 16.0f).loopBeats == 0.0f);
}

TEST_CASE("Transition presets: the same shape at any length", "[app][transition][presets]")
{
    for (int i = 0; i < app::kTransitionPresetCount; ++i)
    {
        const auto p = static_cast<TransitionPreset>(i);
        const auto shortOne = app::makePreset(p, 4.0f), longOne = app::makePreset(p, 128.0f);
        for (int k = 0; k < 2; ++k)
            for (int l = 0; l < engine::kLaneCount; ++l)
                for (const double f : {0.0, 0.1, 0.33, 0.5, 0.74, 0.9, 1.0})
                {
                    const auto& a = (k == 0 ? shortOne.from : shortOne.to)[static_cast<std::size_t>(l)];
                    const auto& b = (k == 0 ? longOne.from : longOne.to)[static_cast<std::size_t>(l)];
                    CHECK(std::abs(evaluate(a, 4.0 * f) - evaluate(b, 128.0 * f)) < 1e-4f);
                }
        // Back onto 0..1: the preset's shape.
        const auto unit = app::unitShape(app::makePreset(p, 24.0f));
        const auto shape = app::presetShape(p);
        for (int l = 0; l < engine::kLaneCount; ++l)
            for (const double f : {0.2, 0.6})
                CHECK(std::abs(evaluate(unit.from[static_cast<std::size_t>(l)], f) - evaluate(shape.from[static_cast<std::size_t>(l)], f)) < 1e-5f);
    }
}

TEST_CASE("Transition presets: a user preset saved and loaded back, at any length", "[app][transition][presets]")
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("devildj-transitions-" + juce::String(std::chrono::steady_clock::now().time_since_epoch().count()));
    // An edited Bass swap at 24 beats: a mid dip added on the new track, the swap moved to beat 18.
    auto mine = app::makePreset(TransitionPreset::BassSwap, 24.0f);
    mine.lane(true, Lane::EqMid).points = {{0.0f, -8.0f, engine::Shape::SCurve}, {12.0f, 0.0f}};
    mine.lane(false, Lane::EqLow).points = {{0.0f, 0.0f, engine::Shape::Hold}, {18.0f, -1000.0f}};
    mine.loopBeats = 2.0f;
    CHECK(app::saveUserPreset(mine, "  My swap  ", dir).isEmpty());
    CHECK(app::saveUserPreset(mine, "   ", dir).isNotEmpty()); // a name is needed
    const auto loaded = app::loadUserPresets(dir);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].name == "My swap");
    CHECK(loaded[0].shape.loopBeats == 2.0f);
    // Stretched back to 24 beats it is what was saved; at 48 the same shape, twice as long.
    const auto again = app::scaled(loaded[0].shape, 24.0f), twice = app::scaled(loaded[0].shape, 48.0f);
    for (int k = 0; k < 2; ++k)
        for (int l = 0; l < engine::kLaneCount; ++l)
        {
            const auto& m = (k == 0 ? mine.from : mine.to)[static_cast<std::size_t>(l)];
            const auto& a = (k == 0 ? again.from : again.to)[static_cast<std::size_t>(l)];
            const auto& b = (k == 0 ? twice.from : twice.to)[static_cast<std::size_t>(l)];
            CHECK(m.points.size() == a.points.size());
            for (double beat = 0.0; beat <= 24.0; beat += 0.5)
            {
                CHECK(std::abs(evaluate(m, beat) - evaluate(a, beat)) < 1e-3f);
                CHECK(std::abs(evaluate(m, beat) - evaluate(b, beat * 2.0)) < 1e-3f);
            }
        }
    // Not a transition file: ignored.
    dir.getChildFile("other.json").replaceWithText("{\"format\": \"something else\"}");
    dir.getChildFile("broken.json").replaceWithText("{ nope");
    CHECK(app::loadUserPresets(dir).size() == 1);
    engine::TransitionCurves junk;
    juce::String name;
    CHECK_FALSE(app::curvesFromJson("[1, 2]", junk, name));
    dir.deleteRecursively();
}
