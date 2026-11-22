// P3-20: full default keymap + editor. Done when: every action in Actions.h is bindable; export/import round-trip test.

#include "ui/Actions.h"
#include "ui/KeyboardController.h"
#include "ui/KeymapEditor.h"
#include "ui/Keymaps.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace devildj;
using namespace devildj::ui;

namespace
{
std::vector<std::string> describe(std::vector<KeyBinding> b)
{
    std::vector<std::string> out;
    for (const auto& x : b)
        out.push_back(commandKey(x.command) + "=" + describeChord(x.chord) + "@" + contextName(x.context));
    std::sort(out.begin(), out.end());
    return out;
}
} // namespace

TEST_CASE("Keymap: every action can be bound and then fires from its key", "[ui][keys][keymap]")
{
    const auto commands = allCommands();
    REQUIRE(static_cast<int>(commands.size()) == kCommandCount);
    ActionDispatcher dispatcher;
    const KeyChord chord{scan::F12, static_cast<std::uint8_t>(mods::Ctrl | mods::Alt | mods::Shift)};
    for (const auto c : commands)
    {
        INFO(commandKey(c));
        const auto bound = assignKey(defaultKeymap(), c, chord).bindings;
        KeyboardController k(dispatcher, bound);
        const auto got = k.lookup(chord, KeyContext::Main);
        REQUIRE(got.has_value());
        CHECK(*got == c);
        CHECK(findConflicts(bound).empty());
        // And it has a stable name for files.
        CHECK(commandFromKey(commandKey(c)) == c);
    }
}

TEST_CASE("Keymap: export / import round trip; old override files; the presets", "[ui][keys][keymap]")
{
    // The full map out and back: the very same bindings.
    for (const auto preset : {KeymapPreset::Default, KeymapPreset::LaptopCompact})
    {
        const auto map = presetKeymap(preset);
        std::vector<std::string> errors;
        const auto back = keymapFromJson(keymapToJson(map), errors);
        CHECK(errors.empty());
        CHECK(describe(back) == describe(map));
    }
    // An edited map (a key moved, one cleared) too.
    auto edited = assignKey(defaultKeymap(), deckCommand(0, DeckAction::Cue), *parseChord("Ctrl+Shift+F12")).bindings;
    edited = clearKey(std::move(edited), globalCommand(GlobalAction::Quantize));
    std::vector<std::string> errors;
    CHECK(describe(keymapFromJson(keymapToJson(edited), errors)) == describe(edited));
    // A P1-16 style override file still works (on top of the default map).
    const auto overrides = keymapFromJson(R"({"version": 1, "bindings": [{"action": "deckA.Cue", "key": "Shift+F12"}]})", errors);
    CHECK(keysOf(overrides, deckCommand(0, DeckAction::Cue)) == "Shift+F12");
    CHECK(keysOf(overrides, deckCommand(0, DeckAction::Play)) == "X");
    // Laptop compact: no F1-F10 left, the F-key actions on Alt + the number row, nothing clashes.
    const auto laptop = presetKeymap(KeymapPreset::LaptopCompact);
    for (const auto& b : laptop)
    {
        CHECK(b.chord.scanCode != scan::F1);
        CHECK(b.chord.scanCode != scan::F10);
    }
    CHECK(keysOf(laptop, globalCommand(GlobalAction::PadModeHotCue)) == "Alt+1");
    CHECK(keysOf(laptop, globalCommand(GlobalAction::Record)) == "Alt+9");
    CHECK(keysOf(laptop, globalCommand(GlobalAction::PadModeKeyShift)) == "Alt+Shift+1");
    CHECK(findConflicts(laptop).empty());
    CHECK(findConflicts(defaultKeymap()).empty());
    CHECK(std::string(presetName(KeymapPreset::LaptopCompact)) == "Laptop compact");
}

TEST_CASE("Keymap editor: filter, pick, press a key (taking it from another), clear, default, preset, save",
          "[ui][keys][keymap]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    KeymapEditor editor(defaultKeymap());
    editor.setSize(720, 560);
    CHECK(editor.visibleRows() == kCommandCount);
    editor.setFilter("loop");
    CHECK(editor.visibleRows() < kCommandCount);
    CHECK(editor.visibleRows() > 5);
    editor.setFilter("");
    // SET KEY on deck A's cue, then Z ... which is already deck A's cue; then X, which deck A's play has.
    editor.selectCommand(deckCommand(0, DeckAction::Cue));
    REQUIRE(editor.selected() == deckCommand(0, DeckAction::Cue));
    editor.setKeyButton().onClick();
    CHECK(editor.isCapturing());
    editor.captureChord({scan::X, mods::None});
    CHECK_FALSE(editor.isCapturing());
    CHECK(keysOf(editor.bindings(), deckCommand(0, DeckAction::Cue)) == "X");
    CHECK(keysOf(editor.bindings(), deckCommand(0, DeckAction::Play)).empty());
    CHECK(editor.message().contains("taken from")); // the conflict is said
    // DEFAULT brings the cue's own key back; CLEAR takes it away.
    editor.defaultButton().onClick();
    CHECK(keysOf(editor.bindings(), deckCommand(0, DeckAction::Cue)) == "Z");
    editor.clearButton().onClick();
    CHECK(keysOf(editor.bindings(), deckCommand(0, DeckAction::Cue)).empty());
    // A preset replaces the map; SAVE hands it to the app.
    editor.presetBox().setSelectedId(3, juce::sendNotificationSync);
    CHECK(keysOf(editor.bindings(), globalCommand(GlobalAction::PadModeHotCue)) == "Alt+1");
    std::vector<KeyBinding> saved;
    editor.onSave = [&](const std::vector<KeyBinding>& b) { saved = b; };
    editor.saveButton().onClick();
    CHECK(describe(saved) == describe(presetKeymap(KeymapPreset::LaptopCompact)));
    juce::Image image(juce::Image::ARGB, 720, 560, true);
    juce::Graphics g(image);
    editor.paintEntireComponent(g, false);
}
