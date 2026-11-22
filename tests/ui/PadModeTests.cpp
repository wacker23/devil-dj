// P3-06: pad engine + pad grid. Done when: each mode works from keyboard and mouse; pad colours reflect state.
// The keyboard and the on-screen pads both end in ui::padCommands (MainComponent::padAction), so the commands
// are tested once here, the engine's side of them, and the grid's looks and mouse path in DeckView.

#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "ui/Actions.h"
#include "ui/DeckView.h"
#include "ui/KeyboardController.h"
#include "ui/PadModes.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace devildj;
using ui::PadMode;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

struct Rig
{
    engine::TrackBuffer track;
    engine::AudioEngine engine;
    float l[kBlock], r[kBlock];
    float* outs[2] = {l, r};
    util::Beatgrid grid = util::Beatgrid::constant(0.5 * kRate, 120.0, kRate);
    Rig()
    {
        track.sampleRate = kRate;
        track.audio.setSize(2, static_cast<int>(60.0 * kRate));
        track.audio.clear();
        engine.prepare(kRate, kBlock);
        engine.send(engine::LoadTrack{0, &track, 1});
        engine.send(engine::SetBeatgrid{0, grid});
        block();
    }
    void block() { engine.process(outs, 2, kBlock); }
    const engine::DeckState& deck() { return engine.readState().decks[0]; }
    // What a pad does, as the app sends it.
    void pad(PadMode mode, int p, bool release = false, bool alternate = false)
    {
        for (const auto& c : ui::padCommands(mode, 0, p, 0, alternate, release, deck()))
            engine.send(c);
        block();
    }
};
} // namespace

TEST_CASE("Pads: the layouts", "[ui][pads]")
{
    CHECK(ui::beatLoopBeats(0) == 0.25);
    CHECK(ui::beatLoopBeats(7) == 32.0);
    CHECK(ui::beatJumpBeats(0) == -1.0);
    CHECK(ui::beatJumpBeats(1) == 1.0);
    CHECK(ui::beatJumpBeats(6) == -8.0);
    CHECK(ui::padSemitones(0) == 0);
    CHECK(ui::padSemitones(3) == 3);
    CHECK(ui::padSemitones(4) == -4);
    CHECK(ui::padSemitones(7) == -1);
    CHECK(ui::padModeAvailable(PadMode::Slicer)); // P3-07
    CHECK(ui::padModeAvailable(PadMode::PadFx));  // P3-12 (the app plays its presets)
    CHECK(ui::padCommands(PadMode::PadFx, 0, 2, 0, false, false, {}).empty());
    CHECK_FALSE(ui::padModeAvailable(PadMode::Sampler));
    CHECK(ui::padCommands(PadMode::Sampler, 0, 2, 0, false, false, {}).empty());
    // F1-F4 and Shift+F1-F4 select the modes.
    ui::ActionDispatcher d;
    ui::KeyboardController k(d);
    const auto f = [&](std::uint16_t code, std::uint8_t m) { return k.lookup({code, m}, ui::KeyContext::Main); };
    CHECK(f(ui::scan::F2, ui::mods::None) == ui::globalCommand(ui::GlobalAction::PadModeBeatLoop));
    CHECK(f(ui::scan::F2, ui::mods::Shift) == ui::globalCommand(ui::GlobalAction::PadModeKeyboard));
    CHECK(ui::findConflicts(ui::defaultKeymap()).empty());
}

TEST_CASE("Pads: beat loop, beat jump, key shift, keyboard drive the deck", "[ui][pads][engine]")
{
    Rig rig;
    rig.engine.send(engine::Seek{0, rig.grid.sampleAt(10.0)});
    rig.engine.send(engine::Play{0});
    rig.block();
    SECTION("beat loop: start, resize from its start, same pad exits")
    {
        rig.pad(PadMode::BeatLoop, 2); // 1 beat
        REQUIRE(rig.deck().loopActive);
        CHECK(std::abs(rig.deck().loopBeats - 1.0) < 1e-6);
        CHECK(ui::padLook(PadMode::BeatLoop, 2, rig.deck(), false).lit);
        CHECK_FALSE(ui::padLook(PadMode::BeatLoop, 3, rig.deck(), false).lit);
        const double start = rig.deck().loopStart;
        rig.pad(PadMode::BeatLoop, 4); // 4 beats, same start
        CHECK(std::abs(rig.deck().loopBeats - 4.0) < 1e-6);
        CHECK(rig.deck().loopStart == start);
        rig.pad(PadMode::BeatLoop, 4);
        CHECK_FALSE(rig.deck().loopActive);
        CHECK_FALSE(ui::padLook(PadMode::BeatLoop, 4, rig.deck(), false).lit);
    }
    SECTION("beat jump")
    {
        const double before = rig.deck().positionSamples;
        rig.pad(PadMode::BeatJump, 5); // +4 beats
        CHECK(std::abs(rig.deck().positionSamples - (before + 4.0 * 24000.0 + kBlock)) < 1.0);
        rig.pad(PadMode::BeatJump, 0); // -1
        CHECK(std::abs(rig.deck().positionSamples - (before + 3.0 * 24000.0 + 2.0 * kBlock)) < 1.0);
        CHECK(ui::padLook(PadMode::BeatJump, 0, rig.deck(), true).lit); // lit while held
        CHECK_FALSE(ui::padLook(PadMode::BeatJump, 0, rig.deck(), false).lit);
    }
    SECTION("key shift: key lock on, the semitones; Shift + pad = 0")
    {
        rig.pad(PadMode::KeyShift, 6); // -2
        CHECK(rig.deck().keyLock);
        CHECK(rig.deck().keyShift == -2);
        CHECK(ui::padLook(PadMode::KeyShift, 6, rig.deck(), false).lit);
        CHECK_FALSE(ui::padLook(PadMode::KeyShift, 0, rig.deck(), false).lit);
        rig.pad(PadMode::KeyShift, 6, false, true);
        CHECK(rig.deck().keyShift == 0);
        CHECK(ui::padLook(PadMode::KeyShift, 0, rig.deck(), false).lit);
    }
    SECTION("keyboard: hot cue A at each pitch; from pause it plays while held")
    {
        rig.pad(PadMode::Keyboard, 1); // A empty: the press sets it here (no pitch yet)
        rig.pad(PadMode::Keyboard, 1, true);
        REQUIRE(rig.deck().hotCueStart[0] >= 0.0);
        CHECK(rig.deck().keyShift == 0);
        const double a = rig.deck().hotCueStart[0];
        rig.engine.send(engine::Pause{0});
        rig.engine.send(engine::Seek{0, 30.0 * kRate});
        rig.block();
        rig.pad(PadMode::Keyboard, 2); // +2: plays A while held
        CHECK(rig.deck().keyLock);
        CHECK(rig.deck().keyShift == 2);
        CHECK(rig.deck().playing);
        CHECK(std::abs(rig.deck().positionSamples - (a + kBlock)) < 1e-6);
        CHECK(ui::padLook(PadMode::Keyboard, 2, rig.deck(), false).lit);
        rig.pad(PadMode::Keyboard, 2, true); // released: back to A, paused
        CHECK_FALSE(rig.deck().playing);
        CHECK(rig.deck().positionSamples == a);
        rig.pad(PadMode::Keyboard, 7); // -1
        CHECK(rig.deck().keyShift == -1);
    }
    SECTION("hot cue mode: the same as the hot cue pads (set, delete with Shift)")
    {
        rig.pad(PadMode::HotCue, 3);
        rig.pad(PadMode::HotCue, 3, true);
        CHECK(rig.deck().hotCueStart[3] >= 0.0);
        rig.pad(PadMode::HotCue, 3, false, true);
        CHECK(rig.deck().hotCueStart[3] < 0.0);
    }
}

TEST_CASE("Pads: the grid shows the mode; colours follow the state; the mouse plays them", "[ui][pads][deckview]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    Rig rig;
    ui::DeckView view(rig.engine, 0, [](engine::BufferId) { return nullptr; });
    view.setBounds(0, 0, 700, 330);
    int pressed = -1, released = -1, menus = 0;
    view.onHotCuePad = [&](int pad, bool down) { (down ? pressed : released) = pad; };
    view.onPadModeMenu = [&] { ++menus; };

    view.setPadMode(PadMode::BeatLoop);
    CHECK(view.padModeButton().getButtonText() == "BEAT LOOP");
    rig.engine.send(engine::Seek{0, rig.grid.sampleAt(10.0)});
    rig.engine.send(engine::Play{0});
    rig.engine.send(engine::Loop{0, engine::LoopOp::Beat, 8.0});
    rig.block();
    view.refresh();
    CHECK(view.padButton(5).getButtonText() == "8");
    const auto lit = view.padButton(5).findColour(juce::TextButton::buttonColourId);
    const auto dim = view.padButton(4).findColour(juce::TextButton::buttonColourId);
    CHECK(lit == juce::Colour(0xff3cc46a)); // the playing size: full colour
    CHECK(dim != lit);
    rig.engine.send(engine::Loop{0, engine::LoopOp::Exit});
    rig.block();
    view.refresh();
    CHECK(view.padButton(5).findColour(juce::TextButton::buttonColourId) == dim);

    // A key held lights a beat-jump pad; the mouse presses / releases any mode's pad.
    view.setPadMode(PadMode::BeatJump);
    view.setPadHeld(1, true);
    CHECK(view.padButton(1).findColour(juce::TextButton::buttonColourId) == juce::Colour(0xff3c7ce0));
    view.setPadHeld(1, false);
    CHECK(view.padButton(1).findColour(juce::TextButton::buttonColourId) != juce::Colour(0xff3c7ce0));
    view.padButton(3).setState(juce::Button::buttonDown);
    view.padButton(3).setState(juce::Button::buttonNormal);
    CHECK(pressed == 3);
    CHECK(released == 3);

    // Not-yet modes: the pads are off; the mode button opens the menu.
    view.setPadMode(PadMode::Sampler);
    CHECK_FALSE(view.padButton(0).isEnabled());
    view.padModeButton().onClick();
    CHECK(menus == 1);
    // Back to hot cues: the page in the button.
    view.setPadMode(PadMode::HotCue);
    CHECK(view.padModeButton().getButtonText() == "HOT CUE A-H");
    juce::Image image(juce::Image::ARGB, 700, 330, true);
    juce::Graphics g(image);
    view.paintEntireComponent(g, false);
}
