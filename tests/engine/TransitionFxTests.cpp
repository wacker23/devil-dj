// P4-18: effects and loops in transitions. Done when: tests for the FX lane and loop timing; listening check.

#include "app/TransitionPresets.h"
#include "dsp/fx/BeatFx.h"
#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "ui/TransitionEditor.h"
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

struct Rig
{
    AudioEngine e;
    std::unique_ptr<TrackBuffer> a = sine(300.0), b = sine(2093.0);
    std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
    Rig()
    {
        e.prepare(kRate, kBlock);
        e.send(LoadTrack{0, a.get(), 1});
        e.send(LoadTrack{1, b.get(), 2});
        for (const DeckId d : {DeckId{0}, DeckId{1}})
            e.send(SetBeatgrid{d, util::Beatgrid::constant(0.0, 120.0, kRate)});
        e.send(Play{0});
        // The incoming deck never heard (its fader down): only the outgoing one and its effect are measured.
        e.send(SetParam{channelParam(1, ChannelParam::Fader), 0.0f});
        render(200);
    }
    void render(int n)
    {
        float* outs[] = {l.data(), r.data()};
        for (int i = 0; i < n; ++i)
            e.process(outs, 2, kBlock);
    }
    // The output's peak over the next `blocks` blocks.
    float peak(int blocks)
    {
        float p = 0.0f;
        float* outs[] = {l.data(), r.data()};
        for (int i = 0; i < blocks; ++i)
        {
            e.process(outs, 2, kBlock);
            for (const float v : l)
                p = std::max(p, std::abs(v));
        }
        return p;
    }
    double beat() { return e.readState().masterBeat; }
    void until(double target)
    {
        while (beat() < target)
            render(1);
    }
};

// The outgoing deck cut at beat 2 of a 4-beat transition, its FX lane full from the start; the incoming deck
// silent all through (so only the outgoing deck and its effect are heard).
std::shared_ptr<TransitionCurves> cutWithSend(int fx)
{
    auto c = std::make_shared<TransitionCurves>();
    c->beats = 4.0f;
    c->fx = {static_cast<std::int8_t>(fx), -1};
    c->fxBeats = 0.5f;
    c->lane(false, Lane::Volume).points = {{0.0f, 1.0f, Shape::Hold}, {2.0f, 0.0f}};
    c->lane(false, Lane::FxSend).points = {{0.0f, 1.0f}};
    c->lane(true, Lane::Volume).points = {{0.0f, 0.0f}};
    return c;
}
} // namespace

TEST_CASE("Transition FX: an echo out rings on after the deck is cut, and after the transition", "[engine][transition][fx]")
{
    // Without an effect: silence once cut.
    {
        Rig rig;
        const double start = std::ceil(rig.beat()) + 1.0;
        rig.e.startTransition(StartTransition{0, 1, 0.0f, TransitionStyle::Blend, start}, cutWithSend(-1));
        rig.until(start + 2.2);
        CHECK(rig.peak(20) < 1e-4f);
    }
    // With the echo: still sounding a beat after the cut, and after the transition has ended.
    Rig rig;
    const double start = std::ceil(rig.beat()) + 1.0;
    rig.e.startTransition(StartTransition{0, 1, 0.0f, TransitionStyle::Blend, start}, cutWithSend(static_cast<int>(dsp::fx::EffectType::Echo)));
    rig.until(start + 1.0);
    const float during = rig.peak(10); // the deck and its echo
    rig.until(start + 2.6);
    const float afterCut = rig.peak(10);
    rig.until(start + 4.5);
    CHECK_FALSE(rig.e.readState().transition.active);
    CHECK_FALSE(rig.e.readState().decks[0].playing);
    const float afterEnd = rig.peak(10);
    WARN("peaks: during " << during << ", after the cut " << afterCut << ", after the end " << afterEnd);
    CHECK(during > 0.3f);
    CHECK(afterCut > 0.05f);
    CHECK(afterEnd > 0.01f);
    CHECK(afterEnd < afterCut); // decaying
    // Long after: silent again (the tail has run out).
    rig.render(static_cast<int>(20.0 * kRate / kBlock));
    CHECK(rig.peak(4) < 1e-3f);
}

TEST_CASE("Transition FX: the lane is the send - at 0 nothing reaches the effect", "[engine][transition][fx]")
{
    Rig rig;
    auto c = cutWithSend(static_cast<int>(dsp::fx::EffectType::Reverb));
    c->lane(false, Lane::FxSend).points = {{0.0f, 0.0f}};
    const double start = std::ceil(rig.beat()) + 1.0;
    rig.e.startTransition(StartTransition{0, 1, 0.0f, TransitionStyle::Blend, start}, c);
    rig.until(start + 2.3);
    CHECK(rig.peak(20) < 1e-4f);
    CHECK(rig.e.readState().transition.lanes[5] == 0.0f);
}

TEST_CASE("Transition loop out: the outgoing deck loops the bar before the start, on the beat, and lets go after",
          "[engine][transition][loop]")
{
    Rig rig;
    auto c = std::make_shared<TransitionCurves>();
    c->beats = 8.0f;
    c->loopBeats = 4.0f;
    c->lane(false, Lane::Volume).points = {{0.0f, 1.0f}, {8.0f, 0.0f}};
    c->lane(true, Lane::Volume).points = {{0.0f, 0.0f}, {8.0f, 1.0f}};
    const auto& s0 = rig.e.readState();
    const double start = std::ceil(s0.masterBeat) + 2.0;
    const double deckStartBeat = s0.decks[0].beat + (start - s0.masterBeat); // where deck A is when it starts
    rig.e.startTransition(StartTransition{0, 1, 0.0f, TransitionStyle::Blend, start}, c);
    const double loopEnd = std::round(deckStartBeat) * 24000.0, loopStart = loopEnd - 4.0 * 24000.0;
    rig.until(start + 0.1);
    int wraps = 0;
    double last = rig.e.readState().decks[0].positionSamples;
    while (rig.e.readState().transition.active)
    {
        rig.render(1);
        const auto& s = rig.e.readState();
        if (!s.transition.active)
            break;
        const double pos = s.decks[0].positionSamples;
        CHECK(pos >= loopStart - 1.0);
        CHECK(pos <= loopEnd + 1.0);
        if (pos < last)
        {
            ++wraps;
            // Wrapped from the end to the start: a jump of one loop, less a block.
            CHECK(std::abs((last - pos) - (4.0 * 24000.0 - kBlock)) < 2.0 * kBlock);
        }
        last = pos;
    }
    CHECK(wraps == 2); // 8 beats of a 4-beat loop
    CHECK_FALSE(rig.e.readState().decks[0].loopActive);
}

TEST_CASE("Transition FX in presets, files and the editor", "[app][transition][fx]")
{
    const auto echo = app::makePreset(app::TransitionPreset::EchoOut, 16.0f);
    CHECK(echo.fx[0] == static_cast<std::int8_t>(dsp::fx::EffectType::Echo));
    CHECK(echo.fx[1] == -1);
    // Saved and read back.
    juce::String name;
    TransitionCurves back;
    REQUIRE(app::curvesFromJson(app::curvesToJson(echo, "x"), back, name));
    CHECK(back.fx == echo.fx);
    CHECK(back.fxBeats == echo.fxBeats);
    // The editor: F the effect (and a send lane to start from), Shift+F its time, O the loop.
    juce::ScopedJuceInitialiser_GUI gui;
    ui::TransitionEditor ed;
    ed.setBounds(0, 0, 1400, 460);
    ui::TransitionEditor::Edit e;
    e.curves = app::makePreset(app::TransitionPreset::SmoothBlend, 32.0f);
    ed.setEdit(e);
    using GA = ui::GlobalAction;
    ed.perform(GA::TransFxType);
    CHECK(ed.edit().curves.fx[0] == static_cast<std::int8_t>(dsp::fx::EffectType::Echo));
    CHECK(ed.edit().curves.lane(false, Lane::FxSend).used());
    ed.perform(GA::TransFxType);
    CHECK(ed.edit().curves.fx[0] == static_cast<std::int8_t>(dsp::fx::EffectType::PingPong));
    ed.perform(GA::TransSide);
    ed.perform(GA::TransFxType);
    CHECK(ed.edit().curves.fx[1] == static_cast<std::int8_t>(dsp::fx::EffectType::Echo));
    const float t0 = ed.edit().curves.fxBeats;
    ed.perform(GA::TransFxTime);
    CHECK(ed.edit().curves.fxBeats != t0);
    ed.perform(GA::TransLoop);
    CHECK(ed.edit().curves.loopBeats == 4.0f);
    ed.perform(GA::TransLoop);
    ed.perform(GA::TransLoop);
    CHECK(ed.edit().curves.loopBeats == 16.0f);
    ed.perform(GA::TransLoop);
    CHECK(ed.edit().curves.loopBeats == 0.0f);
    CHECK(ed.loopButton().getButtonText() == "NO LOOP");
}
