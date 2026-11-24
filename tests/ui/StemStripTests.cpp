// P6-04: a deck's stem buttons - click mutes, Shift solos, right-click the FX send, drag / wheel the level,
// Ctrl+click back to 100 %, WAVE the stem view; nothing without stems.

#include "ui/StemStrip.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using devildj::engine::StemParam;
using devildj::ui::StemStrip;

namespace
{
struct Sent
{
    int stem;
    StemParam param;
    float value;
};

juce::MouseEvent event(StemStrip& s, juce::Point<float> pos, juce::ModifierKeys mods, juce::Point<float> down)
{
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), pos, mods, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            &s, &s, juce::Time::getCurrentTime(), down, juce::Time::getCurrentTime(), 1, false);
}

struct Rig
{
    juce::ScopedJuceInitialiser_GUI gui;
    StemStrip strip;
    std::vector<Sent> sent;
    std::vector<bool> waves;
    devildj::engine::DeckState state;
    Rig()
    {
        strip.setBounds(0, 0, 400, 24);
        strip.onStem = [this](int stem, StemParam p, float v) { sent.push_back({stem, p, v}); };
        strip.onWaveView = [this](bool on) { waves.push_back(on); };
        state.stemsLoaded = true;
        strip.setState(state);
    }
    juce::Point<float> centre(int cell) const { return strip.cellBounds(cell).getCentre(); }
    void click(int cell, juce::ModifierKeys mods = {})
    {
        const auto p = centre(cell);
        strip.mouseDown(event(strip, p, mods, p));
        strip.mouseUp(event(strip, p, mods, p));
    }
};
} // namespace

TEST_CASE("Stem strip: the cells", "[ui][stems]")
{
    Rig r;
    for (int c = 0; c <= StemStrip::kStems; ++c)
        CHECK(r.strip.cellAt(r.centre(c).x) == c);
    CHECK(r.strip.cellBounds(0).getRight() < r.strip.cellBounds(1).getX()); // a gap between them
    CHECK(r.strip.cellBounds(StemStrip::kStems).getRight() == 400.0f);
}

TEST_CASE("Stem strip: click mutes, Shift+click solos, right-click flips the FX send, Ctrl+click is 100 %", "[ui][stems]")
{
    Rig r;
    r.click(3);
    REQUIRE(r.sent.size() == 1);
    CHECK(r.sent[0].stem == 3);
    CHECK(r.sent[0].param == StemParam::Mute);
    CHECK(r.sent[0].value == 1.0f);

    r.state.stemMute = 1u << 3; // what the engine says now: a second click unmutes
    r.strip.setState(r.state);
    r.click(3);
    CHECK(r.sent.back().param == StemParam::Mute);
    CHECK(r.sent.back().value == 0.0f);

    r.click(1, juce::ModifierKeys::shiftModifier);
    CHECK(r.sent.back().stem == 1);
    CHECK(r.sent.back().param == StemParam::Solo);
    CHECK(r.sent.back().value == 1.0f);

    const auto p = r.centre(2);
    r.strip.mouseDown(event(r.strip, p, juce::ModifierKeys::rightButtonModifier, p));
    CHECK(r.sent.back().stem == 2);
    CHECK(r.sent.back().param == StemParam::Fx);
    CHECK(r.sent.back().value == 0.0f); // it was on
    const auto count = r.sent.size();
    r.strip.mouseUp(event(r.strip, p, {}, p)); // the release of a right-click does nothing more
    CHECK(r.sent.size() == count);

    r.click(0, juce::ModifierKeys::ctrlModifier);
    CHECK(r.sent.back().stem == 0);
    CHECK(r.sent.back().param == StemParam::Level);
    CHECK(r.sent.back().value == 1.0f);
}

TEST_CASE("Stem strip: drag and wheel set the level", "[ui][stems]")
{
    Rig r;
    r.state.stemLevel[1] = 0.5f;
    r.strip.setState(r.state);
    const auto p = r.centre(1);
    r.strip.mouseDown(event(r.strip, p, {}, p));
    r.strip.mouseDrag(event(r.strip, p.translated(0.0f, -20.0f), {}, p)); // up 20 px = +25 %
    REQUIRE_FALSE(r.sent.empty());
    CHECK(r.sent.back().param == StemParam::Level);
    CHECK(std::abs(r.sent.back().value - 0.75f) < 1e-6f);
    r.strip.mouseDrag(event(r.strip, p.translated(0.0f, 200.0f), {}, p)); // far down: 0
    CHECK(r.sent.back().value == 0.0f);
    const auto count = r.sent.size();
    r.strip.mouseUp(event(r.strip, p.translated(0.0f, 200.0f), {}, p)); // a drag isn't a click: no mute
    CHECK(r.sent.size() == count);

    juce::MouseWheelDetails wheel{};
    wheel.deltaY = 1.0f;
    r.strip.mouseWheelMove(event(r.strip, p, {}, p), wheel);
    CHECK(r.sent.back().param == StemParam::Level);
    CHECK(std::abs(r.sent.back().value - 0.55f) < 1e-6f);
}

TEST_CASE("Stem strip: WAVE toggles the stem view; without stems nothing is sent", "[ui][stems]")
{
    Rig r;
    r.click(StemStrip::kStems);
    CHECK(r.strip.waveView());
    CHECK(r.waves == std::vector<bool>{true});
    r.click(StemStrip::kStems);
    CHECK_FALSE(r.strip.waveView());

    r.state.stemsLoaded = false;
    r.strip.setState(r.state);
    r.sent.clear();
    r.click(0);
    r.click(StemStrip::kStems);
    CHECK(r.sent.empty());
    CHECK(r.waves.size() == 2);
    CHECK(r.strip.getTooltip().contains("Separate stems"));
}
