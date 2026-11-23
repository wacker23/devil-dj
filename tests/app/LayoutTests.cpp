// P3-23: layouts. Done when: switch layouts while playing without glitches. The layout is UI only (the engine never
// sees it); these check the rectangles of each layout and that an upright waveform maps a finger / mouse along its
// column onto its time axis; switching while playing is checked with the audio running in between.

#include "MainLayout.h"
#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "ui/WaveformView.h"

#include <catch2/catch_test_macros.hpp>

using namespace devildj;

namespace
{
void noOverlaps(const std::vector<juce::Rectangle<int>>& rects)
{
    for (std::size_t i = 0; i < rects.size(); ++i)
        for (std::size_t j = i + 1; j < rects.size(); ++j)
            CHECK_FALSE(rects[i].intersects(rects[j]));
}
} // namespace

TEST_CASE("Layouts: horizontal, vertical and the enlarged browser fit the window without overlapping",
          "[app][layout]")
{
    for (const auto size : {juce::Rectangle<int>(0, 0, 1440, 900), juce::Rectangle<int>(0, 0, 1280, 720),
                            juce::Rectangle<int>(0, 0, 1920, 1080)})
    {
        INFO(size.toString());
        const auto h = app::computeLayout(size, app::WaveLayout::Horizontal, false);
        CHECK(h.wavesVisible);
        CHECK_FALSE(h.wavesVertical);
        CHECK(h.waveA.getWidth() > h.waveA.getHeight()); // across
        CHECK(h.waveA.getBottom() <= h.waveB.getY());     // one above the other
        CHECK(h.browser.getY() >= h.waveB.getBottom());
        noOverlaps({h.deckA, h.deckB, h.mixer, h.waveA, h.waveB, h.panel, h.browser});
        CHECK(size.contains(h.browser));

        const auto v = app::computeLayout(size, app::WaveLayout::Vertical, false);
        CHECK(v.wavesVertical);
        CHECK(v.waveA.getHeight() > v.waveA.getWidth()); // upright
        CHECK(v.waveA.getX() > v.deckA.getRight());       // beside the mixer, between it and the decks
        CHECK(v.waveB.getRight() < v.deckB.getX());
        CHECK(v.browser.getHeight() >= h.browser.getHeight()); // the waveforms' rows go to the browser (or, on small
                                                                // screens, to a taller top row)
        noOverlaps({v.deckA, v.deckB, v.mixer, v.waveA, v.waveB, v.panel, v.browser});

        for (const auto layout : {app::WaveLayout::Horizontal, app::WaveLayout::Vertical})
        {
            const auto big = app::computeLayout(size, layout, true);
            CHECK_FALSE(big.wavesVisible);
            CHECK_FALSE(big.panelVisible);
            CHECK(big.browser.getHeight() > v.browser.getHeight());
        }
    }
}

TEST_CASE("Layouts: an upright waveform's column maps down the column onto its time axis; switching while playing",
          "[app][layout][ui]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    engine::TrackBuffer t;
    t.sampleRate = 48000.0;
    t.audio.setSize(2, 48000 * 20);
    t.audio.clear();
    engine::AudioEngine engine;
    engine.prepare(48000.0, 256);
    engine.send(engine::LoadTrack{0, &t, 1});
    engine.send(engine::Play{0});
    float l[256], r[256];
    float* outs[] = {l, r};
    juce::Component window;
    window.setSize(1440, 900);
    ui::WaveformView wave(engine, 0, [](engine::BufferId) { return nullptr; }, [] { return 0.0; });
    window.addAndMakeVisible(wave);
    const juce::Rectangle<int> column(500, 50, 120, 400);
    for (int round = 0; round < 6; ++round)
    {
        const bool upright = round % 2 == 0;
        if (upright)
        {
            wave.setTransform({});
            wave.setBounds(0, 0, column.getHeight(), column.getWidth());
            wave.setTransform(app::verticalWaveTransform(column));
            // The column's top centre is the view's logical left edge; its bottom, the right edge.
            const auto top = wave.getLocalPoint(&window, juce::Point<int>(column.getCentreX(), column.getY() + 1));
            const auto bottom = wave.getLocalPoint(&window, juce::Point<int>(column.getCentreX(), column.getBottom() - 1));
            CHECK(top.x < 4);
            CHECK(bottom.x > column.getHeight() - 4);
            CHECK(std::abs(top.y - column.getWidth() / 2) <= 2);
        }
        else
        {
            wave.setTransform({});
            wave.setBounds(0, 600, 1400, 120);
            CHECK(wave.getBoundsInParent() == juce::Rectangle<int>(0, 600, 1400, 120));
        }
        juce::Image image(juce::Image::ARGB, 1440, 900, true);
        juce::Graphics g(image);
        window.paintEntireComponent(g, false);
        // The audio goes on in between: the deck keeps playing through every switch.
        for (int b = 0; b < 20; ++b)
            engine.process(outs, 2, 256);
        CHECK(engine.readState().decks[0].playing);
    }
    CHECK(engine.readState().xruns == 0);
}
