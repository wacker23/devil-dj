// P3-10: Color FX (one knob per channel). Done when: manual listening test; parameter smoothing verified.

#include "dsp/fx/ColorFx.h"
#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "ui/MixerView.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

using namespace devildj;
using dsp::fx::ColorFx;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

std::vector<float> sine(int n, double hz = 220.0, double level = 0.5, int offset = 0)
{
    std::vector<float> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] =
            static_cast<float>(level * std::sin(2.0 * 3.141592653589793 * hz * (i + offset) / kRate));
    return v;
}

// One block of a continuous sine through the color FX; returns it.
struct Runner
{
    ColorFx fx;
    int at = 0;
    dsp::fx::BeatContext beat{128.0, 0.0, 128.0 / (60.0 * kRate)};
    Runner(ColorFx::Type type)
    {
        fx.prepare(kRate, kBlock);
        fx.setType(type);
    }
    std::pair<std::vector<float>, std::vector<float>> block(double level = 0.5, double hz = 220.0)
    {
        auto l = sine(kBlock, hz, level, at), r = sine(kBlock, hz, level, at);
        fx.process(l.data(), r.data(), kBlock, beat);
        at += kBlock;
        beat.beatAtBlockStart += kBlock * beat.beatsPerSample;
        return {l, r};
    }
};
} // namespace

TEST_CASE("Color FX: the centre is off - bit-exact - for every character", "[dsp][colorfx]")
{
    for (int t = 0; t < ColorFx::kTypeCount; ++t)
    {
        INFO(ColorFx::typeName(static_cast<ColorFx::Type>(t)));
        Runner run(static_cast<ColorFx::Type>(t));
        for (const double knob : {0.5, 0.49, 0.515}) // inside the dead zone
        {
            run.fx.setKnob(knob);
            for (int b = 0; b < 20; ++b)
            {
                const auto expected = sine(kBlock, 220.0, 0.5, run.at);
                const auto [l, r] = run.block();
                REQUIRE(std::memcmp(l.data(), expected.data(), sizeof(float) * kBlock) == 0);
            }
        }
        CHECK_FALSE(run.fx.isActive());
    }
}

TEST_CASE("Color FX: the amount follows the knob over 20 ms - no steps (smoothing)", "[dsp][colorfx]")
{
    for (const auto type : {ColorFx::Type::Space, ColorFx::Type::DubEcho, ColorFx::Type::Noise, ColorFx::Type::Crush,
                            ColorFx::Type::Pitch})
    {
        INFO(ColorFx::typeName(type));
        Runner run(type);
        run.block();
        run.fx.setKnob(1.0); // a jump, from the centre to full right
        CHECK(run.fx.currentAmount() == 0.0);
        run.block(); // 256 samples of 960
        CHECK(run.fx.currentAmount() > 0.2);
        CHECK(run.fx.currentAmount() < 0.35);
        run.block();
        run.block();
        run.block();
        CHECK(run.fx.currentAmount() == 1.0); // there after 20 ms
        // A jump across the centre: full left, also over 20 ms.
        run.fx.setKnob(0.0);
        run.block();
        CHECK(run.fx.currentAmount() > 0.4);
        CHECK(run.fx.currentAmount() < 0.5);
        for (int b = 0; b < 4; ++b)
            run.block();
        CHECK(run.fx.currentAmount() == -1.0);
    }
    // The mix follows too: a low sine through Crush, knob jumped to the right - the output never jumps more than
    // the sine itself does (the crushed signal only comes in as the amount ramps).
    Runner crush(ColorFx::Type::Crush);
    float previous = 0.0f;
    double worst = 0.0;
    for (int b = 0; b < 12; ++b)
    {
        if (b == 3)
            crush.fx.setKnob(0.62); // a little downsampling
        const auto [l, r] = crush.block(0.5, 40.0);
        for (const float v : l)
        {
            worst = std::max(worst, static_cast<double>(std::abs(v - previous)));
            previous = v;
        }
    }
    CHECK(worst < 0.02);
}

TEST_CASE("Color FX: every character over the whole knob: finite, <= +6 dBFS on 0 dBFS; tails; type changes",
          "[dsp][colorfx]")
{
    for (int t = 1; t < ColorFx::kTypeCount; ++t)
    {
        const auto type = static_cast<ColorFx::Type>(t);
        INFO(ColorFx::typeName(type));
        Runner run(type);
        double worst = 0.0;
        bool finite = true;
        for (double knob = 0.0; knob <= 1.0001; knob += 0.125)
        {
            run.fx.setKnob(knob);
            for (int b = 0; b < 40; ++b)
                for (const double hz : {60.0, 3000.0})
                {
                    const auto [l, r] = run.block(1.0, hz);
                    for (std::size_t i = 0; i < l.size(); ++i)
                    {
                        finite = finite && std::isfinite(l[i]) && std::isfinite(r[i]);
                        worst = std::max({worst, static_cast<double>(std::abs(l[i])), static_cast<double>(std::abs(r[i]))});
                    }
                }
        }
        WARN(ColorFx::typeName(type) << ": peak " << 20.0 * std::log10(worst) << " dBFS");
        CHECK(finite);
        CHECK(worst <= 2.0);
    }
    // Space rings out after the knob is back at the centre, then it is off again (bit-exact).
    Runner space(ColorFx::Type::Space);
    space.fx.setKnob(0.9);
    for (int b = 0; b < 40; ++b)
        space.block();
    space.fx.setKnob(0.5);
    for (int b = 0; b < 10; ++b)
        space.block(0.0);
    const auto [ringing, unused] = space.block(0.0);
    double energy = 0.0;
    for (const float v : ringing)
        energy += v * v;
    CHECK(energy > 1e-6); // still the reverb's tail
    CHECK(space.fx.isActive());
    for (int b = 0; b < 2000 && space.fx.isActive(); ++b)
        space.block(0.0);
    CHECK_FALSE(space.fx.isActive());
    // Changing the character while one plays: it fades out, the new one comes in.
    Runner change(ColorFx::Type::Crush);
    change.fx.setKnob(0.0);
    for (int b = 0; b < 10; ++b)
        change.block();
    change.fx.setType(ColorFx::Type::Pitch);
    change.block();
    for (int b = 0; b < 10; ++b)
        change.block();
    CHECK(change.fx.type() == ColorFx::Type::Pitch);
    CHECK(change.fx.currentAmount() == -1.0); // the knob carried over
}

TEST_CASE("Color FX in the engine and the mixer: the FILTER knob changes job; the label follows", "[engine][colorfx]")
{
    engine::TrackBuffer track;
    track.sampleRate = kRate;
    track.audio.setSize(2, static_cast<int>(5.0 * kRate));
    for (int i = 0; i < track.audio.getNumSamples(); ++i)
    {
        const auto v = static_cast<float>(0.3 * std::sin(2.0 * 3.141592653589793 * 220.0 * i / kRate));
        track.audio.setSample(0, i, v);
        track.audio.setSample(1, i, v);
    }
    engine::AudioEngine e;
    e.prepare(kRate, kBlock);
    e.send(engine::LoadTrack{0, &track, 1});
    e.send(engine::Play{0});
    float l[kBlock], r[kBlock];
    float* outs[] = {l, r};
    e.process(outs, 2, kBlock);
    const auto filter = engine::channelParam(0, engine::ChannelParam::Filter);
    e.send(engine::SetParam{filter, 0.2f});
    e.process(outs, 2, kBlock);
    CHECK(e.readState().channels[0].filter == 0.2f);
    e.send(engine::SetColorFx{static_cast<std::uint8_t>(ColorFx::Type::Crush)});
    e.process(outs, 2, kBlock);
    CHECK(e.readState().colorFx == static_cast<std::uint8_t>(ColorFx::Type::Crush));
    CHECK(e.readState().channels[0].filter == 0.2f); // the knob stays where it is: now it crushes

    juce::ScopedJuceInitialiser_GUI gui;
    ui::MixerView mixer(e);
    mixer.setBounds(0, 0, ui::MixerView::kPreferredWidth, ui::MixerView::kPreferredHeight);
    mixer.refresh();
    CHECK(mixer.colorFxButton().getButtonText() == "CRUSH");
    CHECK(mixer.channel(0).filter->getLabel() == "CRUSH");
    CHECK(ui::MixerView::colorKnobText(static_cast<int>(ColorFx::Type::Pitch), 1.0) == "+12.0 st");
    CHECK(ui::MixerView::colorKnobText(static_cast<int>(ColorFx::Type::Space), 0.0) == "dark 100 %");
    CHECK(ui::MixerView::colorKnobText(static_cast<int>(ColorFx::Type::Noise), 0.5) == "off");
    CHECK(ui::MixerView::colorKnobText(0, 0.5) == "off");
    // Back to the filter: the strip's filter at the knob again.
    e.send(engine::SetColorFx{0});
    e.process(outs, 2, kBlock);
    mixer.refresh();
    CHECK(mixer.channel(0).filter->getLabel() == "FILTER");
}
