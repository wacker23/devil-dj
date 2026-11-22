// P3-21: touch mode. Done when: manual test on a Windows touchscreen (or simulated pointer events). Touch arrives in
// JUCE as mouse events from one source per finger, each going to the component under it; these drive the same
// paths with two "fingers" at once.

#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "ui/DeckView.h"
#include "ui/MixerControls.h"
#include "ui/WaveformView.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace devildj;

TEST_CASE("Touch: two faders moved at once, each by its own finger", "[ui][touch]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ui::ValueControl left("A", ui::ValueControl::Style::VerticalFader, 0.5), right("B", ui::ValueControl::Style::VerticalFader, 0.5);
    left.setBounds(0, 0, 48, 200);
    right.setBounds(60, 0, 48, 200);
    double a = -1.0, b = -1.0;
    left.onChange = [&](double v) { a = v; };
    right.onChange = [&](double v) { b = v; };
    // Two fingers, their moves interleaved: up on one, down on the other.
    for (int i = 0; i < 10; ++i)
    {
        left.dragBy(8.0f, false);
        right.dragBy(-6.0f, false);
    }
    CHECK(a > 0.5);
    CHECK(b < 0.5);
    CHECK(left.getValue() != right.getValue());
}

TEST_CASE("Touch: two pads held at once, let go one by one", "[ui][touch]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    engine::AudioEngine engine;
    engine.prepare(48000.0, 256);
    ui::DeckView view(engine, 0, [](engine::BufferId) { return nullptr; });
    view.setBounds(0, 0, 700, 330);
    std::vector<std::pair<int, bool>> events;
    view.onHotCuePad = [&](int pad, bool down) { events.emplace_back(pad, down); };
    view.padButton(0).setState(juce::Button::buttonDown); // finger 1
    view.padButton(5).setState(juce::Button::buttonDown); // finger 2, while 1 is still down
    view.padButton(0).setState(juce::Button::buttonNormal);
    view.padButton(5).setState(juce::Button::buttonNormal);
    const std::vector<std::pair<int, bool>> expected{{0, true}, {5, true}, {0, false}, {5, false}};
    CHECK(events == expected);
}

TEST_CASE("Touch: dragging a waveform with a finger scratches like the mouse; two decks at once", "[ui][touch]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    engine::TrackBuffer a, b;
    for (auto* t : {&a, &b})
    {
        t->sampleRate = 48000.0;
        t->audio.setSize(2, 48000 * 20);
        t->audio.clear();
    }
    engine::AudioEngine engine;
    engine.prepare(48000.0, 256);
    engine.send(engine::LoadTrack{0, &a, 1});
    engine.send(engine::LoadTrack{1, &b, 2});
    float l[256], r[256];
    float* outs[] = {l, r};
    engine.process(outs, 2, 256);
    ui::WaveformView wa(engine, 0, [](engine::BufferId) { return nullptr; }, [] { return 0.0; });
    ui::WaveformView wb(engine, 1, [](engine::BufferId) { return nullptr; }, [] { return 0.0; });
    wa.setBounds(0, 0, 1200, 120);
    wb.setBounds(0, 130, 1200, 120);
    // Both grabbed, dragged the opposite ways, alternately (two fingers), then let go.
    wa.beginDrag(600.0f);
    wb.beginDrag(600.0f);
    for (int i = 1; i <= 10; ++i)
    {
        wa.dragTo(600.0f - 10.0f * i); // left = forward
        wb.dragTo(600.0f + 5.0f * i);  // right = back (clamped at 0)
        engine.process(outs, 2, 256);
    }
    wa.endDrag();
    wb.endDrag();
    for (int i = 0; i < 20; ++i)
        engine.process(outs, 2, 256);
    CHECK(engine.readState().decks[0].positionSamples > 0.0); // deck A scrubbed forward
    CHECK(engine.readState().decks[1].positionSamples == 0.0); // deck B held at the start
    CHECK_FALSE(engine.readState().decks[0].playing);
    CHECK_FALSE(engine.readState().decks[1].playing);
}

TEST_CASE("Touch mode: the whole UI larger (the global scale)", "[ui][touch]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto& desktop = juce::Desktop::getInstance();
    desktop.setGlobalScaleFactor(1.25f);
    CHECK(desktop.getGlobalScaleFactor() == 1.25f);
    desktop.setGlobalScaleFactor(1.0f);
    CHECK(desktop.getGlobalScaleFactor() == 1.0f);
}
