// P4-17: the transition editor. Done when: everything editable by keyboard and mouse; preview plays the edited
// transition (the app's part is checked by hand; here the editor's side of it).

#include "ui/KeyboardController.h"
#include "ui/Keymaps.h"
#include "ui/TransitionEditor.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace devildj;
using engine::Lane;
using GA = ui::GlobalAction;

namespace
{
analysis::Waveform flatWave(int seconds)
{
    analysis::Waveform w;
    w.sampleRate = 48000.0;
    w.samplesPerPoint = 320;
    const int n = seconds * 150;
    for (int b = 0; b < 3; ++b)
    {
        w.peak[static_cast<std::size_t>(b)].assign(static_cast<std::size_t>(n), 0.3f);
        w.rms[static_cast<std::size_t>(b)].assign(static_cast<std::size_t>(n), 0.2f);
    }
    return w;
}

struct Fixture
{
    juce::ScopedJuceInitialiser_GUI gui;
    analysis::Waveform outWave = flatWave(200), inWave = flatWave(200);
    ui::TransitionEditor editor;
    ui::TransitionEditor::Edit latest;
    int changes = 0;
    Fixture()
    {
        editor.setBounds(0, 0, 1400, 460);
        const auto grid = util::Beatgrid::constant(0.0, 120.0, 48000.0);
        editor.setTracks({&outWave, grid, 200 * 48000, "Out"}, {&inWave, grid, 200 * 48000, "In"});
        ui::TransitionEditor::Edit e;
        e.curves.beats = 32.0f;
        e.curves.lane(false, Lane::Volume).points = {{0.0f, 1.0f}, {32.0f, 0.0f}};
        e.curves.lane(true, Lane::Volume).points = {{0.0f, 0.0f}, {16.0f, 1.0f}};
        e.outBeat = 160.0;
        e.inBeat = 0.0;
        e.preset = "Smooth blend";
        e.reasons = {"a test"};
        editor.setPresetNames({"Smooth blend", "Bass swap"}, {"Mine"});
        editor.setEdit(e);
        editor.onChanged = [this](const ui::TransitionEditor::Edit& x)
        {
            latest = x;
            ++changes;
        };
    }
    std::vector<engine::CurvePoint>& points(bool in, Lane l) { return const_cast<std::vector<engine::CurvePoint>&>(editor.edit().curves.lane(in, l).points); }
    const std::vector<engine::CurvePoint>& pts(bool in, Lane l) const { return editor.edit().curves.lane(in, l).points; }
};
} // namespace

TEST_CASE("Transition editor by keyboard: lanes, sides, the cursor, points, values, shapes, length, moves",
          "[ui][transition][editor]")
{
    Fixture f;
    auto& e = f.editor;
    // The mid EQ of the incoming track: a point at beat 8, down to -3 dB, an S-curve.
    e.perform(GA::TransLane3);
    CHECK(e.lane() == Lane::EqMid);
    e.perform(GA::TransSide);
    CHECK(e.incomingSide());
    e.perform(GA::TransCursorBarRight);
    e.perform(GA::TransCursorBarRight);
    CHECK(e.cursor() == 8.0);
    e.perform(GA::TransValueDown); // no point there: one added, then down
    e.perform(GA::TransValueDown);
    e.perform(GA::TransValueDown);
    REQUIRE(f.pts(true, Lane::EqMid).size() == 1);
    CHECK(f.pts(true, Lane::EqMid)[0].beat == 8.0f);
    CHECK(f.pts(true, Lane::EqMid)[0].value == -3.0f);
    e.perform(GA::TransShape);
    e.perform(GA::TransShape);
    e.perform(GA::TransShape);
    CHECK(f.pts(true, Lane::EqMid)[0].shape == engine::Shape::SCurve);
    // Another at beat 12 (Enter), then from the start the next-point key finds them.
    for (int i = 0; i < 4; ++i)
        e.perform(GA::TransCursorRight);
    e.perform(GA::TransAddPoint);
    CHECK(f.pts(true, Lane::EqMid).size() == 2);
    e.perform(GA::TransCursorBarLeft);
    e.perform(GA::TransCursorBarLeft);
    e.perform(GA::TransCursorBarLeft);
    CHECK(e.cursor() == 0.0);
    e.perform(GA::TransPointNext);
    CHECK(e.cursor() == 8.0);
    e.perform(GA::TransPointNext);
    CHECK(e.cursor() == 12.0);
    e.perform(GA::TransDeletePoint);
    CHECK(f.pts(true, Lane::EqMid).size() == 1);
    e.perform(GA::TransPointPrevious);
    CHECK(e.cursor() == 8.0);
    // Down past the bottom of an EQ lane: kill; up again: back to its range.
    for (int i = 0; i < 40; ++i)
        e.perform(GA::TransValueDown);
    CHECK(f.pts(true, Lane::EqMid)[0].value <= -100.0f);
    e.perform(GA::TransValueUp);
    CHECK(f.pts(true, Lane::EqMid)[0].value == -26.0f);
    // Volume values stay 0..1.
    e.perform(GA::TransLane1);
    e.perform(GA::TransSide); // back to the outgoing deck
    for (int i = 0; i < 30; ++i)
        e.perform(GA::TransValueUp);
    const auto vol = f.pts(false, Lane::Volume);
    for (const auto& p : vol)
        CHECK(p.value <= 1.0f);
    // Length 32 -> 48 bars? no: 8 bars -> the next length (12): the curves stretch.
    CHECK(e.bars() == 8);
    e.perform(GA::TransLength);
    CHECK(e.bars() == 12);
    CHECK(f.pts(true, Lane::Volume).back().beat == 24.0f); // 16 of 32 -> 24 of 48
    CHECK(f.latest.curves.beats == 48.0f);
    // The tracks moved against each other.
    e.perform(GA::TransMoveInLater);
    CHECK(e.edit().inBeat == -1.0);
    e.perform(GA::TransMoveInEarlier);
    e.perform(GA::TransMoveInEarlier);
    CHECK(e.edit().inBeat == 1.0);
    e.perform(GA::TransMoveOutLater);
    CHECK(e.edit().outBeat == 164.0);
    CHECK(e.edit().preset == "Custom");
    CHECK(f.changes > 10);
    // Presets, Auto and preview reach the app.
    std::vector<int> chosen;
    bool autoAsked = false;
    double previewOut = 0.0;
    e.onPreset = [&](int i) { chosen.push_back(i); };
    e.onAuto = [&] { autoAsked = true; };
    e.onPreview = [&](const ui::TransitionEditor::Edit& x) { previewOut = x.outBeat; };
    e.perform(GA::TransPresetNext);
    e.presetBox().setSelectedId(3, juce::sendNotificationSync); // "Mine"
    e.perform(GA::TransAuto);
    e.perform(GA::TransPreview);
    CHECK(chosen == std::vector<int>{0, 2});
    CHECK(autoAsked);
    CHECK(previewOut == 164.0);
    juce::Image image(juce::Image::ARGB, 1400, 460, true);
    juce::Graphics g(image);
    e.paintEntireComponent(g, false);
}

TEST_CASE("Transition editor by mouse: add, drag, delete, shape, move a track, lane buttons, length box",
          "[ui][transition][editor]")
{
    Fixture f;
    auto& e = f.editor;
    // A filter point on the outgoing track at beat 10, half way up (high-pass +0).
    e.laneButton(4).onClick();
    CHECK(e.lane() == Lane::Filter);
    const auto top = e.half(false);
    e.pressAt({e.xOf(10.1), e.yOf(false, Lane::Filter, 0.5f)}, {});
    e.release();
    REQUIRE(f.pts(false, Lane::Filter).size() == 1);
    CHECK(f.pts(false, Lane::Filter)[0].beat == 10.0f); // snapped to a quarter beat
    CHECK(std::abs(f.pts(false, Lane::Filter)[0].value - 0.5f) < 0.05f);
    // Dragged to beat 14, all the way down.
    e.pressAt({e.xOf(10.0), e.yOf(false, Lane::Filter, 0.5f)}, {});
    e.dragTo({e.xOf(14.0), top.getBottom() - 1}, {});
    e.release();
    CHECK(f.pts(false, Lane::Filter)[0].beat == 14.0f);
    CHECK(f.pts(false, Lane::Filter)[0].value == -1.0f);
    // Double-click: its shape.
    const juce::Point<int> at{e.xOf(14.0), e.yOf(false, Lane::Filter, -1.0f)};
    e.mouseDoubleClick(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at.toFloat(), {}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                        &e, &e, juce::Time::getCurrentTime(), at.toFloat(), juce::Time::getCurrentTime(), 2, false));
    CHECK(f.pts(false, Lane::Filter)[0].shape == engine::Shape::EaseIn);
    // Right-click: gone.
    e.pressAt(at, juce::ModifierKeys::rightButtonModifier);
    CHECK(f.pts(false, Lane::Filter).empty());
    // A point in the bottom half goes to the incoming deck's lane.
    e.pressAt({e.xOf(6.0), e.yOf(true, Lane::Filter, 0.0f)}, {});
    e.release();
    CHECK(e.incomingSide());
    CHECK(f.pts(true, Lane::Filter).size() == 1);
    // Shift-drag the bottom half: the incoming track moves by whole beats.
    e.pressAt({e.xOf(4.0), e.half(true).getCentreY()}, juce::ModifierKeys::shiftModifier);
    e.dragTo({e.xOf(7.0), e.half(true).getCentreY()}, juce::ModifierKeys::shiftModifier);
    e.release();
    CHECK(e.edit().inBeat == -3.0);
    // ... the top half by bars.
    e.pressAt({e.xOf(4.0), top.getCentreY()}, juce::ModifierKeys::shiftModifier);
    e.dragTo({e.xOf(-4.2), top.getCentreY()}, juce::ModifierKeys::shiftModifier);
    e.release();
    CHECK(e.edit().outBeat == 168.0);
    // The length box.
    e.lengthBox().setSelectedId(4, juce::sendNotificationSync);
    CHECK(e.bars() == 4);
    CHECK(f.pts(false, Lane::Volume).back().beat == 16.0f);
    // Nothing to edit: a message, no crash.
    e.setTracks({}, {});
    juce::Image image(juce::Image::ARGB, 1400, 460, true);
    juce::Graphics g(image);
    e.paintEntireComponent(g, false);
}

TEST_CASE("Transition editor keys: every action on a key in its own context, none clash", "[ui][transition][keys]")
{
    const auto map = ui::defaultKeymap();
    for (int a = static_cast<int>(GA::TransLane1); a <= static_cast<int>(GA::TransLoop); ++a)
    {
        INFO(ui::commandKey(ui::globalCommand(static_cast<GA>(a))));
        CHECK_FALSE(ui::keysOf(map, ui::globalCommand(static_cast<GA>(a)), ui::KeyContext::Transition).empty());
    }
    CHECK(ui::findConflicts(map).empty());
    CHECK(ui::contextName(ui::KeyContext::Transition) == "transition");
    CHECK(ui::parseContext("transition") == ui::KeyContext::Transition);
    ui::ActionDispatcher d;
    ui::KeyboardController k(d);
    CHECK(k.lookup({ui::scan::Space, ui::mods::None}, ui::KeyContext::Transition) == ui::globalCommand(GA::TransPreview));
    CHECK(k.lookup({ui::scan::Tab, ui::mods::Ctrl}, ui::KeyContext::Transition) == ui::globalCommand(GA::SwitchPanel));
}
