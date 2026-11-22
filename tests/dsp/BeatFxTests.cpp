// P3-09: the beat FX set. Done when: each effect has a parameter-sweep test (no NaN/Inf, no output > +6 dBFS on
// 0 dBFS input) and a manual listening note (DEVLOG).

#include "dsp/fx/BeatFx.h"
#include "dsp/fx/FxSlot.h"
#include "engine/AudioEngine.h"
#include "ui/FxPanel.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <vector>

using namespace devildj;
using dsp::fx::EffectType;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

// 0 dBFS test material: a 60 Hz sine, a 5 kHz sine, full-scale noise, a full-scale square, then silence.
std::vector<float> testSignal(int n, int channel)
{
    std::vector<float> v(static_cast<std::size_t>(n));
    std::uint32_t seed = 777u + static_cast<std::uint32_t>(channel);
    for (int i = 0; i < n; ++i)
    {
        const int part = 5 * i / n;
        const double t = i / kRate;
        float x = 0.0f;
        switch (part)
        {
            case 0:
                x = static_cast<float>(std::sin(2.0 * 3.141592653589793 * 60.0 * t));
                break;
            case 1:
                x = static_cast<float>(std::sin(2.0 * 3.141592653589793 * 5000.0 * t));
                break;
            case 2:
                seed = seed * 1664525u + 1013904223u;
                x = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
                break;
            case 3:
                x = std::fmod(t * 110.0, 1.0) < 0.5 ? 1.0f : -1.0f;
                break;
            default:
                break;
        }
        v[static_cast<std::size_t>(i)] = x;
    }
    return v;
}

struct Sweep
{
    double worst = 0.0;
    bool finite = true;
};

void runThrough(dsp::fx::Effect& fx, double bpm, Sweep& result, int samples)
{
    auto l = testSignal(samples, 0), r = testSignal(samples, 1);
    dsp::fx::BeatContext beat;
    beat.bpm = bpm;
    beat.beatsPerSample = bpm / (60.0 * kRate);
    for (int done = 0; done < samples; done += kBlock)
    {
        const int n = std::min(kBlock, samples - done);
        fx.process(l.data() + done, r.data() + done, n, beat);
        beat.beatAtBlockStart += n * beat.beatsPerSample;
    }
    for (std::size_t i = 0; i < l.size(); ++i)
    {
        result.finite = result.finite && std::isfinite(l[i]) && std::isfinite(r[i]);
        result.worst = std::max({result.worst, static_cast<double>(std::abs(l[i])), static_cast<double>(std::abs(r[i]))});
    }
}

// Process an impulse / a tone and hand back the left output.
std::vector<float> render(dsp::fx::Effect& fx, const std::vector<float>& in, double bpm = 128.0)
{
    auto l = in, r = in;
    dsp::fx::BeatContext beat;
    beat.bpm = bpm;
    beat.beatsPerSample = bpm / (60.0 * kRate);
    for (std::size_t done = 0; done < l.size(); done += kBlock)
    {
        const int n = static_cast<int>(std::min<std::size_t>(kBlock, l.size() - done));
        fx.process(l.data() + done, r.data() + done, n, beat);
        beat.beatAtBlockStart += n * beat.beatsPerSample;
    }
    return l;
}

std::vector<float> tone(int n, double hz = 440.0, double level = 0.5)
{
    std::vector<float> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] = static_cast<float>(level * std::sin(2.0 * 3.141592653589793 * hz * i / kRate));
    return v;
}

double rms(const std::vector<float>& v, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (std::size_t i = from; i < to && i < v.size(); ++i)
        sum += static_cast<double>(v[i]) * v[i];
    return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(1, to - from)));
}
} // namespace

TEST_CASE("Beat FX: every effect, swept over its parameters, tempi and beat lengths: finite, <= +6 dBFS",
          "[dsp][fx][beatfx]")
{
    const int samples = static_cast<int>(0.6 * kRate);
    for (int t = 0; t < dsp::fx::kEffectTypeCount; ++t)
    {
        const auto type = static_cast<EffectType>(t);
        auto fx = dsp::fx::makeEffect(type);
        REQUIRE(fx != nullptr);
        fx->prepare(kRate, kBlock);
        const int np = fx->numParams();
        // Parameter sets: all 0, all 1, the defaults, and each one alone at 0 and at 1 (the others at their defaults).
        std::vector<std::vector<float>> sets;
        std::vector<float> defaults;
        for (int i = 0; i < np; ++i)
            defaults.push_back(fx->param(i).defaultValue);
        sets.push_back(std::vector<float>(static_cast<std::size_t>(np), 0.0f));
        sets.push_back(std::vector<float>(static_cast<std::size_t>(np), 1.0f));
        sets.push_back(defaults);
        for (int i = 0; i < np; ++i)
            for (const float v : {0.0f, 1.0f})
            {
                auto s = defaults;
                s[static_cast<std::size_t>(i)] = v;
                sets.push_back(s);
            }
        Sweep sweep;
        for (const auto [bpm, beats] : {std::pair{60.0, 1.0 / 16.0}, std::pair{128.0, 1.0}, std::pair{200.0, 16.0}})
            for (const auto& set : sets)
            {
                fx->reset();
                fx->setBeats(beats);
                for (int i = 0; i < np; ++i)
                    fx->setParam(i, set[static_cast<std::size_t>(i)]);
                runThrough(*fx, bpm, sweep, samples);
            }
        INFO(fx->name());
        WARN(fx->name() << ": " << sets.size() * 3 << " settings, peak " << 20.0 * std::log10(std::max(sweep.worst, 1e-9))
                        << " dBFS");
        CHECK(sweep.finite);
        CHECK(sweep.worst <= 2.0); // +6 dBFS
        CHECK(std::string(dsp::fx::effectTypeName(type)) == fx->name());
    }
}

TEST_CASE("Beat FX: echo on the beat, roll repeats, trans gates, brake stops, freeze holds", "[dsp][fx][beatfx]")
{
    const double beat = 60.0 / 128.0 * kRate; // 22500 samples
    SECTION("echo: the first repeat one beat later, then half as loud (feedback 0.5, tone open)")
    {
        auto fx = dsp::fx::makeEffect(EffectType::Echo);
        fx->prepare(kRate, kBlock);
        fx->setBeats(1.0);
        fx->setParam(0, 0.5f / 0.9f);
        fx->setParam(1, 1.0f);
        std::vector<float> in(static_cast<std::size_t>(3 * beat + 2000), 0.0f);
        in[100] = 0.5f;
        const auto out = render(*fx, in);
        std::size_t peak = 0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(1.5 * beat); ++i)
            if (std::abs(out[i]) > std::abs(out[peak]))
                peak = i;
        CHECK(std::abs(static_cast<double>(peak) - (100.0 + beat)) <= 2.0);
        double second = 0.0;
        for (std::size_t i = static_cast<std::size_t>(1.5 * beat); i < static_cast<std::size_t>(2.5 * beat); ++i)
            second = std::max(second, static_cast<double>(std::abs(out[i])));
        CHECK(second > 0.15 * std::abs(out[peak]));
        CHECK(second < 0.75 * std::abs(out[peak]));
        CHECK(fx->hasTail());
    }
    SECTION("roll: from the next 1/4-beat boundary, that quarter beat over and over")
    {
        auto fx = dsp::fx::makeEffect(EffectType::Roll);
        fx->prepare(kRate, kBlock);
        fx->setBeats(0.25);
        const auto in = tone(static_cast<int>(4 * beat), 331.0);
        const auto out = render(*fx, in);
        const auto q = static_cast<std::size_t>(beat / 4.0);
        // After the first boundary (sample q) it records q samples, then repeats them: out[n] == out[n - (q - fade)]
        // one round later.
        const auto fade = static_cast<std::size_t>(0.002 * kRate);
        const std::size_t from = 2 * q + 2 * fade + 50;
        double diff = 0.0;
        for (std::size_t i = from; i < from + 2000; ++i)
            diff = std::max(diff, static_cast<double>(std::abs(out[i] - out[i + (q - fade)])));
        CHECK(diff < 1e-6);
        CHECK(rms(out, from, from + 2000) > 0.2);
    }
    SECTION("trans: on for the first half of each beat, off for the second")
    {
        auto fx = dsp::fx::makeEffect(EffectType::Trans);
        fx->prepare(kRate, kBlock);
        fx->setBeats(1.0);
        fx->setParam(0, 0.5f); // duty 50 %
        const auto out = render(*fx, tone(static_cast<int>(3 * beat)));
        CHECK(rms(out, static_cast<std::size_t>(1.1 * beat), static_cast<std::size_t>(1.4 * beat)) > 0.3);
        CHECK(rms(out, static_cast<std::size_t>(1.6 * beat), static_cast<std::size_t>(1.9 * beat)) < 1e-6);
    }
    SECTION("vinyl brake: slowing for one beat, then silent")
    {
        auto fx = dsp::fx::makeEffect(EffectType::VinylBrake);
        fx->prepare(kRate, kBlock);
        fx->setBeats(1.0);
        const auto out = render(*fx, tone(static_cast<int>(2 * beat)));
        CHECK(rms(out, 0, 2000) > 0.3);
        CHECK(rms(out, static_cast<std::size_t>(1.05 * beat), static_cast<std::size_t>(2 * beat)) == 0.0);
    }
    SECTION("freeze: after one beat the first beat is held")
    {
        auto fx = dsp::fx::makeEffect(EffectType::Freeze);
        fx->prepare(kRate, kBlock);
        fx->setBeats(0.5);
        auto in = tone(static_cast<int>(4 * beat), 220.0);
        for (std::size_t i = static_cast<std::size_t>(beat); i < in.size(); ++i)
            in[i] = 0.0f; // silence after the first beat: the held half beat goes on anyway
        const auto out = render(*fx, in);
        CHECK(rms(out, static_cast<std::size_t>(2 * beat), static_cast<std::size_t>(3 * beat)) > 0.2);
    }
    SECTION("bit crush at 2 bits: only a few levels")
    {
        auto fx = dsp::fx::makeEffect(EffectType::BitCrush);
        fx->prepare(kRate, kBlock);
        fx->setParam(0, 1.0f);
        fx->setParam(1, 0.0f);
        const auto out = render(*fx, tone(4800, 440.0, 0.9));
        for (const float v : out)
            REQUIRE(std::abs(v * 2.0f - std::round(v * 2.0f)) < 1e-6f);
    }
    SECTION("tails: the delays and the reverb have one, the rest don't")
    {
        for (int t = 0; t < dsp::fx::kEffectTypeCount; ++t)
        {
            const auto type = static_cast<EffectType>(t);
            const bool expected = type == EffectType::Echo || type == EffectType::PingPong ||
                                  type == EffectType::Reverb || type == EffectType::Spiral;
            CHECK(dsp::fx::makeEffect(type)->hasTail() == expected);
        }
    }
}

TEST_CASE("FX panel: effects, ON, beats, tap, target", "[ui][fx]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    engine::AudioEngine engine;
    engine.prepare(kRate, 256);
    float l[256], r[256];
    float* outs[] = {l, r};
    ui::FxPanel panel(engine);
    panel.setBounds(0, 0, 1200, ui::FxPanel::kPreferredHeight);
    int chosen = -2;
    panel.onEffect = [&](int unit, int slot, int type)
    {
        chosen = type;
        engine.installEffect(unit, slot, type >= 0 ? dsp::fx::makeEffect(static_cast<EffectType>(type)) : nullptr);
        panel.setEffectType(unit, slot, type);
    };
    panel.onEffect(0, 0, static_cast<int>(EffectType::Echo));
    engine.process(outs, 2, 256);
    panel.refresh();
    CHECK(panel.unit(0).slots[0].effect.getButtonText() == "Echo");
    CHECK(panel.unit(0).slots[0].on.isEnabled());
    CHECK(panel.unit(0).slots[0].knobs[0]->getTooltip() == "Feedback");
    // ON, F5 (the unit), F6 (next effect).
    panel.unit(0).slots[0].on.onClick();
    engine.process(outs, 2, 256);
    CHECK(engine.readState().fx[0].slots[0].on);
    panel.toggleUnit(0);
    engine.process(outs, 2, 256);
    CHECK_FALSE(engine.readState().fx[0].slots[0].on);
    panel.nextEffect(0);
    CHECK(chosen == static_cast<int>(EffectType::PingPong));
    // Beats: up / down through the steps.
    panel.unit(0).beatsUp.onClick();
    engine.process(outs, 2, 256);
    CHECK(engine.readState().fx[0].slots[0].beats == 2.0f);
    panel.unit(0).beatsDown.onClick();
    panel.unit(0).beatsDown.onClick();
    engine.process(outs, 2, 256);
    CHECK(engine.readState().fx[0].slots[1].beats == 0.5f);
    CHECK(panel.unit(0).beats.getButtonText() == "1/2");
    // Tap at 120 BPM.
    for (int i = 0; i < 4; ++i)
        panel.tap(1, 10.0 + 0.5 * i);
    engine.process(outs, 2, 256);
    CHECK(std::abs(engine.readState().fx[1].manualBpm - 120.0f) < 0.01f);
    // Target: deck A -> deck B -> master -> deck A.
    panel.cycleTarget(1); // master (the default) -> deck A
    engine.process(outs, 2, 256);
    CHECK(engine.readState().fx[1].target == engine::FxTarget::Deck0);
    panel.cycleTarget(1);
    engine.process(outs, 2, 256);
    CHECK(engine.readState().fx[1].target == engine::FxTarget::Deck1);
    panel.refresh();
    CHECK(panel.unit(1).target.getButtonText() == "FX2: DECK B");
    juce::Image image(juce::Image::ARGB, 1200, ui::FxPanel::kPreferredHeight, true);
    juce::Graphics g(image);
    panel.paintEntireComponent(g, false);
    engine.collectEffects();
}
