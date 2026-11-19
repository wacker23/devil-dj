// P1-14: zoomed waveform. Done when: 60 fps with 2 decks (frame time measured: [.perf] test and the F12
// overlay in the app); waveform visibly aligned with audio (manual click-track test, DEVLOG).

#include "analysis/WaveformBuilder.h"
#include "engine/AudioEngine.h"
#include "ui/DeckView.h"
#include "ui/WaveformView.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <memory>
#include <numbers>

using namespace devildj;
using engine::AudioEngine;
using ui::WaveformView;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

std::shared_ptr<engine::TrackBuffer> makeTrack(bool clicks, double seconds = 180.0)
{
    auto t = std::make_shared<engine::TrackBuffer>();
    t->sampleRate = kRate;
    const int n = static_cast<int>(seconds * kRate);
    t->audio.setSize(2, n);
    const double beat = 60.0 / (clicks ? 120.0 : 128.0);
    for (int i = 0; i < n; ++i)
    {
        const double time = i / kRate;
        const double phase = std::fmod(time, beat) / beat;
        double v;
        if (clicks)
            v = phase * beat < 0.002 ? 0.9 * std::sin(2.0 * std::numbers::pi * 3000.0 * time) : 0.0;
        else
        {
            const double kick = std::exp(-phase * 12.0) * std::sin(2.0 * std::numbers::pi * 55.0 * time);
            const double hat = std::fmod(phase + 0.5, 1.0) < 0.06 ? 0.25 * std::sin(i * 2.1) : 0.0;
            const double pad = 0.22 * std::sin(2.0 * std::numbers::pi * 660.0 * time) * (0.6 + 0.4 * std::sin(time));
            v = 0.75 * kick + hat + pad;
        }
        t->audio.setSample(0, i, static_cast<float>(v));
        t->audio.setSample(1, i, static_cast<float>(v));
    }
    t->title = clicks ? "click_120bpm" : "Night Drive (Extended Mix)";
    t->artist = clicks ? "" : "Generated test signal";
    t->tagBpm = clicks ? 120.0 : 128.0;
    t->waveform = analysis::buildWaveform(t->audio.getReadPointer(0), t->audio.getReadPointer(1), n, kRate);
    return t;
}

struct Rig
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::shared_ptr<engine::TrackBuffer> a = makeTrack(false), b = makeTrack(true);
    AudioEngine engine;
    std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
    std::function<std::shared_ptr<const engine::TrackBuffer>(engine::BufferId)> lookup =
        [this](engine::BufferId id) -> std::shared_ptr<const engine::TrackBuffer>
    {
        return id == 1 ? a : id == 2 ? b : nullptr;
    };
    WaveformView waveA{engine, 0, lookup, [] { return 0.0; }};
    WaveformView waveB{engine, 1, lookup, [] { return 0.0; }};

    Rig()
    {
        engine.prepare(kRate, kBlock);
        engine.send(engine::LoadTrack{0, a.get(), 1});
        engine.send(engine::LoadTrack{1, b.get(), 2});
        run(2);
        waveA.setSize(1240, 130);
        waveB.setSize(1240, 130);
    }
    void run(int blocks)
    {
        float* outs[] = {l.data(), r.data()};
        for (int i = 0; i < blocks; ++i)
            engine.process(outs, 2, kBlock);
    }
    const engine::DeckState& deck(int d) { return engine.readState().decks[static_cast<std::size_t>(d)]; }
};
} // namespace

TEST_CASE("Waveform view: playhead extrapolation and latency", "[ui][waveform]")
{
    engine::DeckState s;
    s.loaded = true;
    s.playing = true;
    s.lengthSamples = 480000;
    s.positionSamples = 96000.0;
    s.rate = 1.0;
    // 10 ms after the state: 480 samples further.
    CHECK(std::abs(WaveformView::displayedPosition(s, kRate, 100.0, 100.010, 0.0) - 96480.0) < 1e-6);
    // ... minus 20 ms of output latency.
    CHECK(std::abs(WaveformView::displayedPosition(s, kRate, 100.0, 100.010, 0.020) - 95520.0) < 1e-6);
    // Faster deck moves faster.
    s.rate = 1.08;
    CHECK(std::abs(WaveformView::displayedPosition(s, kRate, 100.0, 100.010, 0.0) - (96000.0 + 1.08 * 480.0)) < 1e-9);
    // Paused: no extrapolation, no latency shift.
    s.playing = false;
    CHECK(WaveformView::displayedPosition(s, kRate, 100.0, 100.5, 0.02) == 96000.0);
    // A stalled engine (old state): extrapolation stops after 100 ms.
    s.playing = true;
    s.rate = 1.0;
    CHECK(std::abs(WaveformView::displayedPosition(s, kRate, 100.0, 105.0, 0.0) - 100800.0) < 1e-6);
    // Never outside the track.
    s.positionSamples = 479990.0;
    CHECK(WaveformView::displayedPosition(s, kRate, 100.0, 100.05, 0.0) == 480000.0);
}

TEST_CASE("Waveform view: zoom steps and scale", "[ui][waveform]")
{
    Rig rig;
    auto& w = rig.waveA;
    CHECK(w.visibleSeconds() == 6.0);
    CHECK(w.samplesPerPixel() == 6.0 * kRate / 1240.0);
    w.zoomBy(1);
    CHECK(w.visibleSeconds() == 4.0);
    w.zoomBy(10);
    CHECK(w.visibleSeconds() == 2.0); // clamped
    w.zoomBy(-20);
    CHECK(w.visibleSeconds() == 32.0);
}

TEST_CASE("Waveform view: drag scratches / scrubs, wheel nudges", "[ui][waveform]")
{
    Rig rig;
    rig.engine.send(engine::Seek{0, 10.0 * kRate});
    rig.run(2);
    auto& w = rig.waveA;
    const double spp = w.samplesPerPixel();

    // Paused: grab and drag 100 px left = scrub 100 px of track forward; stays paused.
    w.beginDrag(600.0f);
    rig.run(1);
    CHECK(rig.deck(0).scratching);
    for (int i = 1; i <= 10; ++i)
    {
        w.dragTo(600.0f - 10.0f * i);
        rig.run(3); // ~16 ms per move
    }
    rig.run(40); // the hand stopped: the deck settles on it
    CHECK(std::abs(rig.deck(0).positionSamples - (10.0 * kRate + 100.0 * spp)) < 0.01 * kRate);
    w.endDrag();
    rig.run(2);
    CHECK_FALSE(rig.deck(0).scratching);
    CHECK_FALSE(rig.deck(0).playing);

    // Wheel: two notches forward = +10 ms (paused: moved silently).
    const double before = rig.deck(0).positionSamples;
    w.nudgeNotches(2.0);
    rig.run(1);
    CHECK(std::abs(rig.deck(0).positionSamples - (before + 0.010 * kRate)) < 1e-6);
}

TEST_CASE("Waveform view: paints with and without a track", "[ui][waveform]")
{
    Rig rig;
    rig.engine.send(engine::Play{0});
    rig.run(10);
    const auto image = rig.waveA.createComponentSnapshot(rig.waveA.getLocalBounds());
    CHECK(image.getWidth() == 1240);
    // The playhead column is white.
    CHECK(image.getPixelAt(620, 65).getBrightness() > 0.95f);

    AudioEngine empty;
    empty.prepare(kRate, kBlock);
    WaveformView none(empty, 3, [](engine::BufferId) { return nullptr; }, [] { return 0.0; });
    none.setSize(400, 100);
    CHECK(none.createComponentSnapshot(none.getLocalBounds()).getWidth() == 400);
}

TEST_CASE("Waveform view frame time (2 decks)", "[.perf]")
{
    Rig rig;
    rig.engine.send(engine::Play{0});
    rig.engine.send(engine::Play{1});
    rig.run(10);
    juce::Image frame(juce::Image::ARGB, 1240, 266, true);
    constexpr int frames = 600;
    double worst = 0.0, total = 0.0;
    for (int f = -10; f < frames; ++f) // 10 warm-up frames (glyph caches) aren't counted
    {
        rig.run(1);
        const auto start = std::chrono::steady_clock::now();
        {
            juce::Graphics g(frame);
            rig.waveA.paintEntireComponent(g, false);
            g.setOrigin(0, 136);
            rig.waveB.paintEntireComponent(g, false);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (f < 0)
            continue;
        total += ms;
        worst = std::max(worst, ms);
    }
    WARN("2 waveforms 1240 x 130, software renderer: " << total / frames << " ms per frame avg, " << worst
                                                       << " ms worst (60 fps budget 16.7 ms)");
    CHECK(total / frames < 8.0);
}

TEST_CASE("Waveform screenshot", "[.screenshot]")
{
    Rig rig;
    rig.engine.send(engine::Seek{0, 0.1 * rig.a->numSamples()});
    rig.engine.send(engine::CuePress{0});
    rig.engine.send(engine::CueRelease{0});
    rig.run(4);
    rig.engine.send(engine::Seek{0, 0.1 * rig.a->numSamples() + 3.0 * kRate});
    rig.engine.send(engine::Seek{1, 41.0 * kRate});
    rig.engine.send(engine::SetTempo{0, 2.0});
    rig.engine.send(engine::Play{0});
    rig.engine.send(engine::Play{1});
    rig.run(20);

    ui::DeckView deckA(rig.engine, 0, rig.lookup), deckB(rig.engine, 1, rig.lookup);
    deckA.setSize(614, 250);
    deckB.setSize(614, 250);
    deckA.refresh();
    deckB.refresh();
    juce::Image image(juce::Image::ARGB, 1264, 548, true);
    {
        juce::Graphics g(image);
        g.fillAll(juce::Colour(0xff101014));
        g.drawImageAt(deckA.createComponentSnapshot(deckA.getLocalBounds()), 12, 12);
        g.drawImageAt(deckB.createComponentSnapshot(deckB.getLocalBounds()), 638, 12);
        g.drawImageAt(rig.waveA.createComponentSnapshot(rig.waveA.getLocalBounds()), 12, 274);
        g.drawImageAt(rig.waveB.createComponentSnapshot(rig.waveB.getLocalBounds()), 12, 410);
    }
    const auto out =
        juce::File(juce::String::fromUTF8(DEVILDJ_SOURCE_DIR)).getChildFile("docs/images/p1-14-waveforms.png");
    out.deleteFile();
    juce::FileOutputStream stream(out);
    REQUIRE(stream.openedOk());
    juce::PNGImageFormat png;
    REQUIRE(png.writeImageToStream(image, stream));
}

TEST_CASE("Waveform view: beat lines from the deck's grid, downbeats in red", "[ui][waveform][grid]")
{
    Rig rig;
    rig.run(2); // paused at the start: sample 0 sits under the playhead (x = 620)
    const double spp = rig.waveA.samplesPerPixel();
    // Downbeat at 0.25 s, 120 BPM: beats every 0.5 s after it.
    auto grid = devildj::util::Beatgrid::constant(0.25 * kRate, 120.0, kRate);
    rig.waveA.setGridSource([&grid] { return grid; });
    const auto image = rig.waveA.createComponentSnapshot(rig.waveA.getLocalBounds());
    const auto columnAt = [&](double sample) { return 620 + static_cast<int>(std::floor(sample / spp)); };
    const auto redNear = [&](int x)
    {
        for (int dx = -1; dx <= 2; ++dx)
        {
            const auto c = image.getPixelAt(x + dx, 2);
            if (c.getRed() > 150 && c.getRed() > 2 * c.getGreen())
                return true;
        }
        return false;
    };
    CHECK(redNear(columnAt(0.25 * kRate)));      // a downbeat
    CHECK_FALSE(redNear(columnAt(0.75 * kRate))); // beat 2: a grey line
    CHECK(redNear(columnAt(2.25 * kRate)));       // the next bar's downbeat (4 beats later)
    // No grid: no red lines.
    grid = {};
    const auto plain = rig.waveA.createComponentSnapshot(rig.waveA.getLocalBounds());
    const auto c = plain.getPixelAt(columnAt(0.25 * kRate), 2);
    CHECK_FALSE((c.getRed() > 150 && c.getRed() > 2 * c.getGreen()));
}