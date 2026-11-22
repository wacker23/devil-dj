// P3-12: Pad FX. Done when: presets persist; keyboard mode triggers them.

#include "PadFxPresets.h"
#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "ui/Actions.h"
#include "ui/DeckView.h"
#include "ui/KeyboardController.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>

using namespace devildj;

TEST_CASE("Pad FX presets: the defaults, saved as JSON and read back; bad input falls back", "[app][padfx]")
{
    const auto defaults = app::defaultPadFxPresets();
    CHECK(app::presetName(defaults[0][0]) == "Echo 1/2");
    CHECK(app::presetName(defaults[0][7]) == "Echo Out");
    CHECK(app::presetName(defaults[1][4]) == "Pitch +7");
    CHECK(defaults[0][7].release);
    CHECK(app::presetColour(defaults[0][7]) != app::presetColour(defaults[0][0]));

    // Edited presets survive the round trip (as they do through the settings file).
    auto edited = defaults;
    edited[1][2] = app::presetFor(false, static_cast<int>(dsp::fx::EffectType::BitCrush));
    edited[1][2].beats = 0.25f;
    edited[1][2].wet = 0.75f;
    edited[1][2].latch = true;
    edited[1][2].params = {1.0f, 0.25f, 0.0f};
    edited[0][3] = app::presetFor(true, static_cast<int>(dsp::fx::ReleaseFx::Kind::FilterSweep));
    const auto back = app::presetsFromJson(app::toJson(edited));
    for (int b = 0; b < app::kPadFxBanks; ++b)
        for (int p = 0; p < app::kPadFxPads; ++p)
        {
            const auto& x = edited[static_cast<std::size_t>(b)][static_cast<std::size_t>(p)];
            const auto& y = back[static_cast<std::size_t>(b)][static_cast<std::size_t>(p)];
            INFO("bank " << b << " pad " << p);
            CHECK(x.release == y.release);
            CHECK(x.type == y.type);
            CHECK(x.beats == y.beats);
            CHECK(x.wet == y.wet);
            CHECK(x.latch == y.latch);
            CHECK(x.params == y.params);
        }
    // Rubbish, or a bank short of pads, or an effect that doesn't exist: the defaults there.
    CHECK(app::presetName(app::presetsFromJson("not json")[0][0]) == "Echo 1/2");
    const auto partial = app::presetsFromJson(R"([[{"release":false,"type":99}], []])");
    CHECK(app::presetName(partial[0][0]) == "Echo 1/2");
    const auto clamped = app::presetsFromJson(R"([[{"release":false,"type":9,"beats":100,"wet":7}]])");
    CHECK(clamped[0][0].beats == 16.0f);
    CHECK(clamped[0][0].wet == 1.0f);
}

TEST_CASE("Pad FX: what a press does - momentary, latched, release effects; the keys play the pads", "[app][padfx][keys]")
{
    engine::DeckState state;
    auto momentary = app::presetFor(false, static_cast<int>(dsp::fx::EffectType::Echo));
    auto cmds = app::padFxCommands(momentary, 1, 2, false, state);
    REQUIRE(cmds.size() == 1);
    const auto& on = std::get<engine::PadFxCommand>(cmds[0]);
    CHECK(on.deck == 1);
    CHECK(on.pad == 2);
    CHECK(on.op == engine::FxOp::On);
    CHECK(on.value == 1.0f);
    CHECK(std::get<engine::PadFxCommand>(app::padFxCommands(momentary, 1, 2, true, state)[0]).value == 0.0f);

    auto latched = momentary;
    latched.latch = true;
    CHECK(std::get<engine::PadFxCommand>(app::padFxCommands(latched, 0, 4, false, state)[0]).value == 1.0f);
    CHECK(app::padFxCommands(latched, 0, 4, true, state).empty()); // the release does nothing
    state.padFxOn = 1u << 4;
    CHECK(std::get<engine::PadFxCommand>(app::padFxCommands(latched, 0, 4, false, state)[0]).value == 0.0f);

    const auto release = app::defaultPadFxPresets()[0][7]; // echo out
    const auto press = std::get<engine::ReleaseFxCommand>(app::padFxCommands(release, 0, 7, false, state)[0]);
    CHECK(press.press);
    CHECK(press.kind == static_cast<std::uint8_t>(dsp::fx::ReleaseFx::Kind::EchoOut));
    CHECK_FALSE(std::get<engine::ReleaseFxCommand>(app::padFxCommands(release, 0, 7, true, state)[0]).press);

    // Keyboard: the deck rows are pads 1-8 in every mode (Q W E R / A S D F for deck A).
    ui::ActionDispatcher d;
    ui::KeyboardController k(d);
    const auto pad = [&](std::uint16_t code) { return k.lookup({code, ui::mods::None}, ui::KeyContext::Main); };
    CHECK(pad(ui::scan::Q) == ui::deckCommand(0, ui::DeckAction::Pad1));
    CHECK(pad(ui::scan::F) == ui::deckCommand(0, ui::DeckAction::Pad8));
    CHECK(pad(ui::scan::U) == ui::deckCommand(1, ui::DeckAction::Pad1));
    CHECK(k.lookup({ui::scan::F3, ui::mods::Shift}, ui::KeyContext::Main) ==
          ui::globalCommand(ui::GlobalAction::PadModePadFx));
}

TEST_CASE("Pad FX in the engine: a pad's effect on and off; off is bit-exact; the deck shows which", "[engine][padfx]")
{
    constexpr double rate = 48000.0;
    constexpr int block = 256;
    engine::TrackBuffer track;
    track.sampleRate = rate;
    track.audio.setSize(2, static_cast<int>(10.0 * rate));
    for (int i = 0; i < track.audio.getNumSamples(); ++i)
    {
        const auto v = static_cast<float>(0.3 * std::sin(2.0 * 3.141592653589793 * 330.0 * i / rate));
        track.audio.setSample(0, i, v);
        track.audio.setSample(1, i, v);
    }
    engine::AudioEngine plain, withPads;
    for (auto* e : {&plain, &withPads})
    {
        e->prepare(rate, block);
        e->send(engine::LoadTrack{0, &track, 1});
        e->send(engine::Play{0});
    }
    auto crush = dsp::fx::makeEffect(dsp::fx::EffectType::BitCrush);
    crush->setParam(0, 1.0f); // 2 bits
    REQUIRE(withPads.installPadEffect(0, 3, std::move(crush)));
    withPads.send(engine::PadFxCommand{0, 3, engine::FxOp::Wet, 0, 1.0f});
    float pl[block], pr[block], fl[block], fr[block];
    float* p[] = {pl, pr};
    float* f[] = {fl, fr};
    const auto run = [&](int n)
    {
        bool same = true;
        for (int b = 0; b < n; ++b)
        {
            plain.process(p, 2, block);
            withPads.process(f, 2, block);
            same = same && std::memcmp(pl, fl, sizeof(pl)) == 0;
        }
        return same;
    };
    CHECK(run(20)); // installed, off: identical
    withPads.send(engine::PadFxCommand{0, 3, engine::FxOp::On, 0, 1.0f});
    CHECK_FALSE(run(20));
    CHECK(withPads.readState().decks[0].padFxOn == 1u << 3);
    CHECK(withPads.readState().decks[0].padFxActive == 1u << 3);
    withPads.send(engine::PadFxCommand{0, 3, engine::FxOp::On, 0, 0.0f});
    run(10); // 20 ms fade
    CHECK(withPads.readState().decks[0].padFxActive == 0);
    CHECK(run(20)); // off again: identical

    // The deck's pads in Pad FX mode: the presets' names; the playing one lit.
    juce::ScopedJuceInitialiser_GUI gui;
    ui::DeckView view(withPads, 0, [](engine::BufferId) { return nullptr; });
    view.setBounds(0, 0, 700, 330);
    std::array<juce::String, 8> names{"Echo 1/2", "B", "C", "Crush", "E", "F", "G", "Echo Out"};
    std::array<std::uint32_t, 8> colours{};
    colours.fill(0x9A5CE0u);
    std::array<bool, 8> release{};
    release[7] = true;
    view.setPadMode(ui::PadMode::PadFx);
    view.setPadFxLooks(names, colours, release, 1);
    CHECK(view.padModeButton().getButtonText() == "PAD FX 2");
    withPads.send(engine::PadFxCommand{0, 3, engine::FxOp::On, 0, 1.0f});
    run(2);
    view.refresh();
    CHECK(view.padButton(3).getButtonText() == "Crush");
    CHECK(view.padButton(3).findColour(juce::TextButton::buttonColourId) == juce::Colour(0xff9a5ce0));
    CHECK(view.padButton(2).findColour(juce::TextButton::buttonColourId) != juce::Colour(0xff9a5ce0));
    withPads.collectEffects();
}
