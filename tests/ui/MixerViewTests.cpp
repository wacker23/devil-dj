// P1-15: mixer view. Done when: every control works by mouse, wheel and touch.
// Touch reaches JUCE components as mouse events on the same handlers, so the gesture paths below (drag, Alt +
// drag, wheel, double-click / double-tap) cover mouse and touch; each is checked on the engine side.

#include "analysis/WaveformBuilder.h"
#include "engine/AudioEngine.h"
#include "ui/DeckView.h"
#include "ui/MixerView.h"
#include "ui/WaveformView.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>

using namespace devildj;
using Catch::Approx;
using ui::MixerView;
using ui::ValueControl;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

struct Rig
{
    juce::ScopedJuceInitialiser_GUI gui;
    engine::AudioEngine engine;
    std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
    std::unique_ptr<MixerView> mixer;

    Rig()
    {
        engine.prepare(kRate, kBlock);
        run(1);
        mixer = std::make_unique<MixerView>(engine);
        mixer->setSize(MixerView::kPreferredWidth, MixerView::kPreferredHeight);
    }
    void run(int blocks = 2)
    {
        float* outs[] = {l.data(), r.data()};
        for (int i = 0; i < blocks; ++i)
            engine.process(outs, 2, kBlock);
    }
    const engine::EngineState& state() { return engine.readState(); }
    const engine::ChannelState& ch(int i) { return engine.readState().channels[static_cast<std::size_t>(i)]; }
};
} // namespace

TEST_CASE("Mixer mappings: knob positions <-> dB", "[ui][mixer]")
{
    CHECK(MixerView::trimDb(0.0) == -12.0);
    CHECK(MixerView::trimDb(0.5) == 0.0);
    CHECK(MixerView::trimDb(1.0) == 12.0);
    // EQ mode: left = -26 dB, centre 0, right +6.
    CHECK(MixerView::eqDb(0.0, false) == -26.0);
    CHECK(MixerView::eqDb(0.25, false) == -13.0);
    CHECK(MixerView::eqDb(0.5, false) == 0.0);
    CHECK(MixerView::eqDb(1.0, false) == 6.0);
    // Isolator: the left half fades the band out to a full kill.
    CHECK(MixerView::eqDb(0.0, true) <= -100.0);
    CHECK(MixerView::eqDb(0.25, true) == Approx(-6.0206).margin(1e-3));
    CHECK(MixerView::levelDb(0.0) <= -100.0);
    CHECK(MixerView::levelDb(0.5) == 0.0);
    CHECK(MixerView::levelDb(1.0) == 6.0);
    // Round trips.
    for (double k = 0.0; k <= 1.0; k += 0.05)
    {
        CHECK(MixerView::trimKnob(MixerView::trimDb(k)) == Approx(k).margin(1e-9));
        CHECK(MixerView::eqKnob(MixerView::eqDb(k, false), false) == Approx(k).margin(1e-9));
        CHECK(MixerView::eqKnob(MixerView::eqDb(k, true), true) == Approx(k).margin(1e-9));
        CHECK(MixerView::levelKnob(MixerView::levelDb(k)) == Approx(k).margin(1e-9));
    }
    CHECK(MixerView::eqKnob(-std::numeric_limits<double>::infinity(), true) == 0.0); // a kill shows fully left
    CHECK(MixerView::eqKnob(-std::numeric_limits<double>::infinity(), false) == 0.0);
}

TEST_CASE("Value control gestures: drag, fine drag, wheel, double-click", "[ui][mixer]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ValueControl knob("TEST", ValueControl::Style::Knob, 0.5);
    knob.setSize(52, 44);
    double sent = -1.0;
    knob.onChange = [&](double v) { sent = v; };

    knob.dragBy(ValueControl::kKnobPixelsPerRange * 0.25f, false); // drag up a quarter of the range
    CHECK(sent == Approx(0.75));
    knob.dragBy(-ValueControl::kKnobPixelsPerRange * 0.25f, true); // Alt: a tenth of that
    CHECK(sent == Approx(0.725));
    knob.wheel(2.0, false); // two notches
    CHECK(sent == Approx(0.765));
    knob.wheel(-1.0, true); // Alt + wheel
    CHECK(sent == Approx(0.763));
    knob.resetToDefault(); // double-click / double-tap
    CHECK(sent == 0.5);
    knob.dragBy(10000.0f, false);
    CHECK(sent == 1.0); // clamped
    sent = -1.0;
    knob.setValue(0.2, false); // updates from the engine don't echo back
    CHECK(sent == -1.0);

    // A fader follows the finger: dragging its full travel moves it across the whole range.
    ValueControl fader("A", ValueControl::Style::VerticalFader, 1.0);
    fader.setSize(48, 124); // travel = height - 24 = 100 px
    fader.onChange = [&](double v) { sent = v; };
    fader.dragBy(-50.0f, false);
    CHECK(sent == Approx(0.5));
    ValueControl xf("", ValueControl::Style::HorizontalFader, 0.5);
    xf.setSize(230, 30); // travel = 200 px
    xf.onChange = [&](double v) { sent = v; };
    xf.dragBy(100.0f, false);
    CHECK(sent == Approx(1.0));
}

TEST_CASE("Mixer view: every control reaches the engine", "[ui][mixer]")
{
    Rig rig;
    auto& a = rig.mixer->channel(0);
    auto& b = rig.mixer->channel(1);

    a.trim->dragBy(ValueControl::kKnobPixelsPerRange * 0.25f, false); // +6 dB
    a.hi->wheel(-5.0, false);                                         // 0.5 - 0.1 = 0.4 -> -5.2 dB
    a.mid->resetToDefault();
    a.low->dragBy(-ValueControl::kKnobPixelsPerRange, false);            // fully left: -26 dB (EQ mode)
    a.filter->dragBy(-ValueControl::kKnobPixelsPerRange * 0.25f, false); // 0.25: low-pass
    a.fader->dragBy(-0.5f * (a.fader->getHeight() - 24.0f), false);      // half way
    b.cue->onClick();
    rig.mixer->crossfader().wheel(-5.0, false);                                        // towards A: 0.4
    rig.mixer->master().wheel(-5.0, false);                                            // 0.4 -> 20 log10(0.8)
    rig.mixer->headphoneMix().dragBy(ValueControl::kKnobPixelsPerRange * 0.5f, false); // 1.0: master only
    rig.mixer->headphoneLevel().resetToDefault();
    rig.run();

    CHECK(rig.ch(0).trimDb == Approx(6.0f));
    CHECK(rig.ch(0).eqDb[2] == Approx(-5.2f).margin(1e-4));
    CHECK(rig.ch(0).eqDb[1] == 0.0f);
    CHECK(rig.ch(0).eqDb[0] == -26.0f);
    CHECK(rig.ch(0).filter == Approx(0.25f));
    CHECK(rig.ch(0).fader == Approx(0.5f));
    CHECK(rig.ch(1).cue);
    CHECK(rig.state().crossfader == Approx(0.4f));
    CHECK(rig.state().masterGainDb == Approx(20.0 * std::log10(0.8)).margin(1e-4));
    CHECK(rig.state().cueMix == 1.0f);
    CHECK(rig.state().headphoneLevelDb == 0.0f);

    // In isolator mode the low knob fully left is a kill.
    rig.engine.send(engine::SetParam{engine::channelParam(1, engine::ChannelParam::EqMode), 1.0f});
    rig.run();
    b.low->dragBy(-ValueControl::kKnobPixelsPerRange, false);
    rig.run();
    CHECK(std::isinf(rig.ch(1).eqDb[0]));
}

TEST_CASE("Mixer view: shows changes made elsewhere (keys, menu)", "[ui][mixer]")
{
    Rig rig;
    rig.engine.send(engine::SetParam{engine::channelParam(0, engine::ChannelParam::EqMode), 1.0f});
    rig.engine.send(engine::SetParam{engine::channelParam(0, engine::ChannelParam::EqLow), -1000.0f}); // Ctrl+A kill
    rig.engine.send(engine::SetParam{engine::ParamId::Crossfader, 1.0f});                              // Shift+H
    rig.engine.send(engine::SetParam{engine::channelParam(1, engine::ChannelParam::Cue), 1.0f});       // Ctrl+O
    rig.run();
    rig.mixer->refresh();
    CHECK(rig.mixer->channel(0).low->getValue() == 0.0);
    CHECK(rig.mixer->crossfader().getValue() == 1.0);
    CHECK(rig.mixer->channel(1).cue->getToggleState());
    CHECK(rig.mixer->channel(0).fader->getValue() == 1.0);
}

TEST_CASE("Mixer screenshot", "[.screenshot]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    engine::AudioEngine engine;
    engine.prepare(kRate, kBlock);
    auto track = std::make_shared<engine::TrackBuffer>();
    track->sampleRate = kRate;
    const int n = static_cast<int>(200.0 * kRate);
    track->audio.setSize(2, n);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kRate, beat = 60.0 / 126.0, ph = std::fmod(t, beat) / beat;
        const double v = 0.7 * std::exp(-ph * 10.0) * std::sin(2.0 * std::numbers::pi * 52.0 * t) +
                         0.2 * std::sin(2.0 * std::numbers::pi * 700.0 * t) * (0.5 + 0.5 * std::sin(t * 0.7)) +
                         (std::fmod(ph + 0.5, 1.0) < 0.05 ? 0.2 * std::sin(i * 1.9) : 0.0);
        track->audio.setSample(0, i, static_cast<float>(v));
        track->audio.setSample(1, i, static_cast<float>(v * 0.95));
    }
    track->title = "Basement Lights";
    track->artist = "Generated test signal";
    track->tagBpm = 126.0;
    track->waveform = analysis::buildWaveform(track->audio.getReadPointer(0), track->audio.getReadPointer(1), n, kRate);
    auto lookup = [&](engine::BufferId id) -> std::shared_ptr<const engine::TrackBuffer>
    { return id == 1 || id == 2 ? track : nullptr; };

    engine.send(engine::LoadTrack{0, track.get(), 1});
    engine.send(engine::LoadTrack{1, track.get(), 2});
    engine.send(engine::Seek{0, 64.0 * kRate});
    engine.send(engine::Seek{1, 20.0 * kRate});
    engine.send(engine::Play{0});
    engine.send(engine::Play{1});
    using P = engine::ChannelParam;
    engine.send(engine::SetParam{engine::channelParam(1, P::EqLow), -14.0f});
    engine.send(engine::SetParam{engine::channelParam(1, P::Filter), 0.62f});
    engine.send(engine::SetParam{engine::channelParam(1, P::Fader), 0.7f});
    engine.send(engine::SetParam{engine::channelParam(1, P::Cue), 1.0f});
    engine.send(engine::SetParam{engine::channelParam(0, P::EqHigh), 3.0f});
    engine.send(engine::SetParam{engine::ParamId::Crossfader, 0.35f});
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[] = {l.data(), r.data()};
    for (int i = 0; i < 120; ++i)
        engine.process(outs, 2, kBlock);

    juce::Component window;
    window.setSize(1280, 730);
    ui::DeckView deckA(engine, 0, lookup), deckB(engine, 1, lookup);
    MixerView mixer(engine);
    ui::WaveformView waveA(engine, 0, lookup, [] { return 0.0; }), waveB(engine, 1, lookup, [] { return 0.0; });
    for (auto* c : std::initializer_list<juce::Component*>{&deckA, &deckB, &mixer, &waveA, &waveB})
        window.addAndMakeVisible(c);
    const int deckWidth = (1256 - MixerView::kPreferredWidth - 12) / 2;
    deckA.setBounds(12, 12, deckWidth, 250);
    mixer.setBounds(12 + deckWidth + 6, 12, MixerView::kPreferredWidth, MixerView::kPreferredHeight);
    deckB.setBounds(1268 - deckWidth, 12, deckWidth, 250);
    waveA.setBounds(12, 12 + MixerView::kPreferredHeight + 8, 1256, 130);
    waveB.setBounds(12, 12 + MixerView::kPreferredHeight + 144, 1256, 130);
    deckA.refresh();
    deckB.refresh();
    mixer.refresh();

    juce::Image image(juce::Image::ARGB, 1280, 730, true);
    {
        juce::Graphics g(image);
        g.fillAll(juce::Colour(0xff101014));
        window.paintEntireComponent(g, false);
    }
    const auto out = juce::File(juce::String::fromUTF8(DEVILDJ_SOURCE_DIR)).getChildFile("docs/images/p1-15-mixer.png");
    out.deleteFile();
    juce::FileOutputStream stream(out);
    REQUIRE(stream.openedOk());
    juce::PNGImageFormat png;
    REQUIRE(png.writeImageToStream(image, stream));
}
