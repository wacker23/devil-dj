// P4-14: transition curves in the engine. Done when: every lane's value at given beats within 0.1 dB / 0.01 of its
// curve; the old auto-transition tests still pass.

#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "engine/TransitionCurves.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <vector>

using namespace devildj;
using namespace devildj::engine;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

std::unique_ptr<TrackBuffer> sine(double hz, double seconds = 60.0)
{
    auto t = std::make_unique<TrackBuffer>();
    t->sampleRate = kRate;
    const int n = static_cast<int>(seconds * kRate);
    t->audio.setSize(2, n);
    for (int i = 0; i < n; ++i)
    {
        const auto v = static_cast<float>(0.4 * std::sin(2.0 * 3.141592653589793 * hz * i / kRate));
        t->audio.setSample(0, i, v);
        t->audio.setSample(1, i, v);
    }
    return t;
}

double toneLevel(const std::vector<float>& x, double hz)
{
    const double w = 2.0 * 3.141592653589793 * hz / kRate, c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (const float v : x)
    {
        const double s0 = v + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / static_cast<double>(x.size());
}

double dB(double ratio)
{
    return 20.0 * std::log10(std::max(1e-9, ratio));
}
} // namespace

TEST_CASE("Transition curves: shapes, before / after, holds, normalizing", "[engine][transition][curves]")
{
    LaneCurve lane;
    CHECK(evaluate(lane, 3.0) == 0.0f); // unused
    lane.points = {{0.0f, 0.0f, Shape::Linear}, {8.0f, 1.0f, Shape::Linear}};
    CHECK(evaluate(lane, -1.0) == 0.0f);
    CHECK(evaluate(lane, 4.0) == 0.5f);
    CHECK(evaluate(lane, 9.0) == 1.0f);
    lane.points[0].shape = Shape::EaseIn;
    CHECK(evaluate(lane, 4.0) == 0.25f);
    lane.points[0].shape = Shape::EaseOut;
    CHECK(evaluate(lane, 4.0) == 0.75f);
    lane.points[0].shape = Shape::SCurve;
    CHECK(evaluate(lane, 4.0) == 0.5f);
    CHECK(evaluate(lane, 2.0) < 0.25f);
    lane.points[0].shape = Shape::Hold;
    CHECK(evaluate(lane, 7.9) == 0.0f);
    CHECK(evaluate(lane, 8.0) == 1.0f);
    // Three points.
    lane.points = {{0.0f, 1.0f}, {4.0f, 1.0f}, {12.0f, 0.0f}};
    CHECK(evaluate(lane, 2.0) == 1.0f);
    CHECK(evaluate(lane, 8.0) == 0.5f);
    // Normalizing: sorted, clamped to the lane's range and the length.
    TransitionCurves c;
    c.beats = 8.0f;
    c.lane(false, Lane::Volume).points = {{9.0f, 2.0f}, {1.0f, -1.0f}};
    c.lane(true, Lane::Filter).points = {{2.0f, -3.0f}};
    normalize(c);
    const auto& v = c.lane(false, Lane::Volume).points;
    CHECK(v[0].beat == 1.0f);
    CHECK(v[0].value == 0.0f);
    CHECK(v[1].beat == 8.0f);
    CHECK(v[1].value == 1.0f);
    CHECK(c.lane(true, Lane::Filter).points[0].value == -1.0f);
    CHECK(neutralValue(Lane::Volume) == 1.0f);
    CHECK(neutralValue(Lane::EqMid) == 0.0f);
}

TEST_CASE("Transition curves in the engine: every lane follows its curve, the channels come back after",
          "[engine][transition][curves]")
{
    AudioEngine e;
    e.prepare(kRate, kBlock);
    auto a = sine(300.0), b = sine(2093.0);
    e.send(LoadTrack{0, a.get(), 1});
    e.send(LoadTrack{1, b.get(), 2});
    for (const DeckId d : {DeckId{0}, DeckId{1}})
        e.send(SetBeatgrid{d, util::Beatgrid::constant(0.0, 120.0, kRate)});
    e.send(SetParam{channelParam(0, ChannelParam::CrossfaderAssign), 0.0f}); // A
    e.send(SetParam{channelParam(1, ChannelParam::CrossfaderAssign), 2.0f}); // B
    e.send(SetParam{ParamId::Crossfader, 0.0f});
    e.send(SetParam{channelParam(0, ChannelParam::EqMid), -3.0f}); // a setting of the DJ's, put back after
    e.send(Play{0});
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[] = {l.data(), r.data()};
    const auto render = [&](int n)
    {
        for (int i = 0; i < n; ++i)
            e.process(outs, 2, kBlock);
    };
    const auto level = [&](double hz)
    {
        std::vector<float> x;
        for (int i = 0; i < 8; ++i)
        {
            e.process(outs, 2, kBlock);
            x.insert(x.end(), l.begin(), l.end());
        }
        return toneLevel(x, hz);
    };
    render(100);
    const double fullA = level(300.0);
    REQUIRE(fullA > 0.1);

    auto curves = std::make_shared<TransitionCurves>();
    curves->beats = 16.0f;
    curves->lane(false, Lane::Volume).points = {{0.0f, 1.0f}, {16.0f, 0.0f}};
    curves->lane(false, Lane::EqLow).points = {{0.0f, 0.0f, Shape::Hold}, {8.0f, -1000.0f}};
    curves->lane(false, Lane::Filter).points = {{8.0f, 0.0f}, {16.0f, 1.0f, Shape::Linear}};
    curves->lane(true, Lane::Volume).points = {{0.0f, 0.0f}, {16.0f, 1.0f}};
    curves->lane(true, Lane::EqHigh).points = {{0.0f, -12.0f, Shape::EaseOut}, {16.0f, 0.0f}};
    curves->lane(true, Lane::FxSend).points = {{0.0f, 0.0f}, {16.0f, 0.5f}};
    const double start = std::ceil(e.readState().masterBeat) + 1.0;
    REQUIRE(e.startTransition(StartTransition{0, 1, 0.0f, TransitionStyle::Blend, start}, curves));

    // Each block: the published lanes are their curves at the block's start; the channels follow.
    const double beatsPerBlock = kBlock * 120.0 / (60.0 * kRate);
    double worstEqDb = 0.0, worstFilter = 0.0, worstLane = 0.0;
    int checked = 0;
    for (int i = 0; i < 2200; ++i)
    {
        e.process(outs, 2, kBlock);
        const auto& s = e.readState();
        if (!s.transition.active || s.transition.waiting)
            continue;
        const double at = s.masterBeat - beatsPerBlock - start; // the block's start, in transition beats
        if (at < 0.0 || at > 16.0)
            continue;
        ++checked;
        for (int k = 0; k < 2; ++k)
            for (int lane = 0; lane < kLaneCount; ++lane)
            {
                const auto& lc = curves->lane(k == 1, static_cast<Lane>(lane));
                if (!lc.used())
                    continue;
                worstLane = std::max(worstLane, std::abs(static_cast<double>(s.transition.lanes[static_cast<std::size_t>(k * 6 + lane)]) -
                                                         evaluate(lc, at)));
            }
        worstEqDb = std::max(worstEqDb, static_cast<double>(std::abs(s.channels[1].eqDb[2] - evaluate(curves->lane(true, Lane::EqHigh), at))));
        if (at < 7.9)
            worstEqDb = std::max(worstEqDb, std::abs(static_cast<double>(s.channels[0].eqDb[0])));
        if (at > 8.1)
            CHECK(s.channels[0].eqDb[0] <= -100.0f); // killed from beat 8
        worstFilter = std::max(worstFilter, std::abs(s.channels[0].filter - (0.5 + 0.5 * evaluate(curves->lane(false, Lane::Filter), at))));
        CHECK(s.transition.curves);
    }
    WARN(checked << " blocks checked: worst lane " << worstLane << ", EQ " << worstEqDb << " dB, filter " << worstFilter);
    CHECK(checked > 300);
    CHECK(worstLane < 0.01);
    CHECK(worstEqDb < 0.1);
    CHECK(worstFilter < 0.01);
    // Finished: A paused, both channels back as they were (EQ mid -3 dB on A), assigns back, the crossfader on B.
    const auto& s = e.readState();
    CHECK_FALSE(s.transition.active);
    CHECK_FALSE(s.decks[0].playing);
    CHECK(s.decks[1].playing);
    CHECK(s.channels[0].eqDb[0] == 0.0f);
    CHECK(s.channels[0].eqDb[1] == -3.0f);
    CHECK(s.channels[0].filter == 0.5f);
    CHECK(s.channels[1].eqDb[2] == 0.0f);
    CHECK(s.channels[0].crossfaderAssign == 0);
    CHECK(s.channels[1].crossfaderAssign == 2);
    CHECK(s.crossfader == 1.0f);
    render(40);
    const double fullB = level(2093.0);
    CHECK(fullB > 0.3);
}

TEST_CASE("Transition curves: the volume lane is the gain heard, within 0.1 dB", "[engine][transition][curves]")
{
    AudioEngine e;
    e.prepare(kRate, kBlock);
    auto a = sine(300.0), b = sine(2093.0);
    e.send(LoadTrack{0, a.get(), 1});
    e.send(LoadTrack{1, b.get(), 2});
    for (const DeckId d : {DeckId{0}, DeckId{1}})
        e.send(SetBeatgrid{d, util::Beatgrid::constant(0.0, 120.0, kRate)});
    e.send(Play{0});
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[] = {l.data(), r.data()};
    for (int i = 0; i < 100; ++i)
        e.process(outs, 2, kBlock);
    const auto level = [&](double hz)
    {
        std::vector<float> x;
        for (int i = 0; i < 4; ++i)
        {
            e.process(outs, 2, kBlock);
            x.insert(x.end(), l.begin(), l.end());
        }
        return toneLevel(x, hz);
    };
    const double fullA = level(300.0);
    // A steady 0.5 on the outgoing deck from beat 0 to 8, then down to 0.
    auto curves = std::make_shared<TransitionCurves>();
    curves->beats = 16.0f;
    curves->lane(false, Lane::Volume).points = {{0.0f, 0.5f}, {8.0f, 0.5f}, {16.0f, 0.0f}};
    curves->lane(true, Lane::Volume).points = {{0.0f, 0.0f}, {4.0f, 0.25f, Shape::Hold}, {16.0f, 1.0f}};
    const double start = std::ceil(e.readState().masterBeat) + 1.0;
    REQUIRE(e.startTransition(StartTransition{0, 1, 0.0f, TransitionStyle::Blend, start}, curves));
    while (e.readState().masterBeat < start + 5.0)
        e.process(outs, 2, kBlock);
    const double halfA = level(300.0), quarterB = level(2093.0);
    // Decks A / B with the crossfader in the middle: A was at its crossfader gain; the lane is relative to that.
    CHECK(std::abs(dB(halfA / fullA) - dB(0.5)) < 0.1);
    // B at 0.25 against its own full level (measured after the transition).
    while (e.readState().transition.active)
        e.process(outs, 2, kBlock);
    for (int i = 0; i < 40; ++i)
        e.process(outs, 2, kBlock);
    const double fullB = level(2093.0);
    CHECK(std::abs(dB(quarterB / fullB) - dB(0.25)) < 0.1);
}

TEST_CASE("Transition curves: cancelled midway, everything as it was", "[engine][transition][curves]")
{
    AudioEngine e;
    e.prepare(kRate, kBlock);
    auto a = sine(300.0), b = sine(2093.0);
    e.send(LoadTrack{0, a.get(), 1});
    e.send(LoadTrack{1, b.get(), 2});
    e.send(Play{0});
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[] = {l.data(), r.data()};
    for (int i = 0; i < 20; ++i)
        e.process(outs, 2, kBlock);
    auto curves = std::make_shared<TransitionCurves>();
    curves->beats = 8.0f;
    curves->lane(false, Lane::EqLow).points = {{0.0f, -1000.0f}};
    curves->lane(false, Lane::Filter).points = {{0.0f, -0.6f}};
    const double start = std::ceil(e.readState().masterBeat) + 1.0;
    e.startTransition(StartTransition{0, 1, 0.0f, TransitionStyle::Blend, start}, curves);
    while (e.readState().masterBeat < start + 3.0)
        e.process(outs, 2, kBlock);
    CHECK(e.readState().channels[0].eqDb[0] <= -100.0f);
    e.send(CancelTransition{});
    e.process(outs, 2, kBlock);
    const auto& s = e.readState();
    CHECK_FALSE(s.transition.active);
    CHECK(s.channels[0].eqDb[0] == 0.0f);
    CHECK(s.channels[0].filter == 0.5f);
    CHECK(s.channels[0].crossfaderAssign == 0); // deck A's own side, as before
    CHECK(s.channels[1].crossfaderAssign == 2);
    CHECK(s.decks[0].playing); // cancelled: the outgoing deck plays on
}
