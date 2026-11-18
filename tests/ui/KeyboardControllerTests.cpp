// P1-16: action system + keyboard controller. Includes the test that fails if two actions share a key and
// modifiers in the same focus context.

#include "ui/Actions.h"
#include "ui/KeyboardController.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

using namespace devildj::ui;

namespace
{
struct Recorder
{
    ActionDispatcher dispatcher;
    std::vector<std::pair<CommandId, ActionPhase>> log;

    Recorder()
    {
        for (auto id : allCommands())
            dispatcher.setHandler(id, [this, id](ActionPhase p) { log.emplace_back(id, p); });
    }
};

KeyboardController::KeyEvent down(std::uint16_t code, std::uint8_t m = mods::None)
{
    return {code, true, false, m};
}
KeyboardController::KeyEvent repeat(std::uint16_t code, std::uint8_t m = mods::None)
{
    return {code, true, true, m};
}
KeyboardController::KeyEvent up(std::uint16_t code, std::uint8_t m = mods::None)
{
    return {code, false, false, m};
}
} // namespace

TEST_CASE("Default keymap: no two actions share a key + modifiers in the same context", "[ui][keys]")
{
    const auto conflicts = findConflicts(defaultKeymap());
    for (const auto& [a, b] : conflicts)
        UNSCOPED_INFO(describeChord(a.chord) << ": " << commandName(a.command) << " / " << commandName(b.command));
    CHECK(conflicts.empty());

    // The check itself works.
    auto broken = defaultKeymap();
    broken.push_back({{scan::Z, mods::None}, globalCommand(GlobalAction::Record), KeyContext::Main});
    CHECK(findConflicts(broken).size() == 1);
    // ... but the same chord in another panel's context is allowed (panel keys override while focused).
    auto panel = defaultKeymap();
    panel.push_back({{scan::Z, mods::Shift}, globalCommand(GlobalAction::Record), KeyContext::CodeBeat});
    CHECK(findConflicts(panel).empty());
}

TEST_CASE("Default keymap matches KEYMAP.md for the core keys", "[ui][keys]")
{
    ActionDispatcher d;
    KeyboardController k(d);
    const auto is = [&](std::uint16_t code, std::uint8_t m, CommandId expected)
    {
        const auto got = k.lookup({code, m}, KeyContext::Main);
        INFO(describeChord({code, m}) << " -> " << (got ? commandName(*got) : std::string("nothing")));
        CHECK((got && *got == expected));
    };
    using DA = DeckAction;
    using GA = GlobalAction;
    is(scan::Z, mods::None, deckCommand(0, DA::Cue));
    is(scan::X, mods::None, deckCommand(0, DA::Play));
    is(scan::M, mods::None, deckCommand(1, DA::Cue));
    is(scan::Comma, mods::None, deckCommand(1, DA::Play));
    is(scan::Digit1, mods::None, deckCommand(0, DA::BendDown));
    is(scan::Digit8, mods::None, deckCommand(1, DA::BendUp));
    is(scan::Digit2, mods::Shift, deckCommand(0, DA::TempoUp));
    is(scan::Digit3, mods::Shift, deckCommand(0, DA::FilterToLowPass));
    is(scan::Minus, mods::Shift, deckCommand(1, DA::FilterReset));
    is(scan::Z, mods::Shift, deckCommand(0, DA::JumpToStart));
    is(scan::A, mods::Ctrl, deckCommand(0, DA::KillLow));
    is(scan::L, mods::Ctrl, deckCommand(1, DA::KillHigh));
    is(scan::W, mods::Ctrl, deckCommand(0, DA::KeyLock));
    is(scan::O, mods::Ctrl, deckCommand(1, DA::HeadphoneCue));
    is(scan::G, mods::None, globalCommand(GA::CrossfaderLeft));
    is(scan::H, mods::Shift, globalCommand(GA::CrossfaderSnapRight));
    is(scan::B, mods::None, globalCommand(GA::CrossfaderCentre));
    is(scan::Tab, mods::None, deckCommand(0, DA::Load));
    is(scan::Enter, mods::None, deckCommand(1, DA::Load));
    is(scan::F12, mods::None, globalCommand(GA::DebugOverlay));
    is(scan::Escape, mods::None, globalCommand(GA::Panic));
    CHECK_FALSE(k.lookup({scan::Space, mods::None}, KeyContext::Main)); // deliberately unbound
}

TEST_CASE("Commands: names, keys and the full list", "[ui][keys]")
{
    const auto all = allCommands();
    CHECK(static_cast<int>(all.size()) == kCommandCount);
    std::set<std::string> keys;
    for (auto id : all)
    {
        CHECK(commandName(id) != "?");
        keys.insert(commandKey(id));
    }
    CHECK(static_cast<int>(keys.size()) == kCommandCount); // stable file ids are unique
    CHECK(commandName(deckCommand(1, DeckAction::Cue)) == "Deck B: Cue (hold to preview)");
    CHECK(commandKey(globalCommand(GlobalAction::CrossfaderCentre)) == "global.CrossfaderCentre");
    CHECK(commandDeck(deckCommand(3, DeckAction::Load)) == 3);
    CHECK(commandDeck(globalCommand(GlobalAction::Panic)) == -1);
    CHECK(commandRepeats(deckCommand(0, DeckAction::TempoUp)));
    CHECK_FALSE(commandRepeats(deckCommand(0, DeckAction::Cue)));
}

TEST_CASE("Keyboard controller: press / release / hold, no key repeat", "[ui][keys]")
{
    Recorder r;
    KeyboardController k(r.dispatcher);
    const auto cue = deckCommand(0, DeckAction::Cue);

    CHECK(k.handle(down(scan::Z)));
    CHECK(k.isHeld(cue));
    CHECK(k.handle(repeat(scan::Z))); // auto-repeat is swallowed, nothing re-triggers
    CHECK(k.handle(repeat(scan::Z)));
    CHECK(k.handle(up(scan::Z)));
    CHECK_FALSE(k.isHeld(cue));
    REQUIRE(r.log.size() == 2);
    CHECK(r.log[0] == std::pair{cue, ActionPhase::Press});
    CHECK(r.log[1] == std::pair{cue, ActionPhase::Release});

    // Repeating actions do get Repeat phases.
    r.log.clear();
    const auto tempo = deckCommand(0, DeckAction::TempoUp);
    k.handle(down(scan::Digit2, mods::Shift));
    k.handle(repeat(scan::Digit2, mods::Shift));
    k.handle(repeat(scan::Digit2, mods::Shift));
    k.handle(up(scan::Digit2, mods::Shift));
    REQUIRE(r.log.size() == 4);
    CHECK(r.log[1] == std::pair{tempo, ActionPhase::Repeat});
    CHECK(r.log[3] == std::pair{tempo, ActionPhase::Release});
}

TEST_CASE("Keyboard controller: a release ends what the press started, even if Shift changed", "[ui][keys]")
{
    Recorder r;
    KeyboardController k(r.dispatcher);
    const auto bend = deckCommand(0, DeckAction::BendDown);
    k.handle(down(scan::Digit1));            // bend (no Shift)
    k.handle(up(scan::Digit1, mods::Shift)); // Shift pressed meanwhile: still ends the bend
    REQUIRE(r.log.size() == 2);
    CHECK(r.log[1] == std::pair{bend, ActionPhase::Release});

    r.log.clear();
    k.handle(down(scan::Digit1, mods::Shift)); // tempo step
    k.handle(up(scan::Digit1));                // Shift let go first
    CHECK(r.log.back() == std::pair{deckCommand(0, DeckAction::TempoDown), ActionPhase::Release});
    CHECK_FALSE(k.isHeld(bend));
}

TEST_CASE("Keyboard controller: modifiers alone, unbound keys, text fields, focus loss", "[ui][keys]")
{
    Recorder r;
    KeyboardController k(r.dispatcher);
    CHECK_FALSE(k.handle(down(scan::LeftShift, mods::Shift)));
    CHECK_FALSE(k.handle(down(scan::Space)));                     // unbound: passed on
    CHECK_FALSE(k.handle(down(scan::Z), KeyContext::Main, true)); // typing in a text field
    CHECK_FALSE(k.handle(up(scan::Z), KeyContext::Main, true));
    CHECK(r.log.empty());

    k.handle(down(scan::G)); // crossfader glide held
    k.handle(down(scan::M)); // deck B cue held
    k.releaseAll();          // window lost focus
    CHECK_FALSE(k.isHeld(globalCommand(GlobalAction::CrossfaderLeft)));
    CHECK(r.log.size() == 4);
    CHECK_FALSE(k.handle(up(scan::G))); // the late release is ignored
}

TEST_CASE("Keyboard controller: a focused panel's keys win over the main view's", "[ui][keys]")
{
    Recorder r;
    KeyboardController k(r.dispatcher);
    k.handle(down(scan::Digit1), KeyContext::BeatMaker);
    CHECK(r.log.back().first == globalCommand(GlobalAction::BeatMakerTrack1)); // not pitch bend
    k.handle(up(scan::Digit1), KeyContext::BeatMaker);
    k.handle(down(scan::Z), KeyContext::BeatMaker); // not a Beat Maker key: the deck cue still works
    CHECK(r.log.back().first == deckCommand(0, DeckAction::Cue));
}

TEST_CASE("Hook decoding: the scan code survives the IME (VK_PROCESSKEY)", "[ui][keys]")
{
    // Z key down as Windows reports it with the Korean IME on: wParam = VK_PROCESSKEY (0xE5), lParam keeps
    // the hardware scan code 0x2C in bits 16-23.
    constexpr std::uintptr_t vkProcessKey = 0xE5;
    const std::intptr_t zDown = (0x2C << 16) | 1;
    const auto e = PhysicalKeyHook::decode(vkProcessKey, zDown, mods::None);
    REQUIRE(e);
    CHECK(e->scanCode == scan::Z);
    CHECK(e->down);
    CHECK_FALSE(e->repeat);

    const auto zRepeat = PhysicalKeyHook::decode('Z', zDown | (1 << 30), mods::None);
    CHECK(zRepeat->repeat);
    const auto zUp = PhysicalKeyHook::decode(
        'Z', static_cast<std::intptr_t>(static_cast<std::uint32_t>(zDown) | (1u << 30) | (1u << 31)), mods::None);
    CHECK_FALSE(zUp->down);
    // Extended keys: Up arrow = 0x48 with the extended flag.
    const auto upArrow = PhysicalKeyHook::decode(0x26, (0x48 << 16) | (1 << 24) | 1, mods::Shift);
    CHECK(upArrow->scanCode == scan::Up);
    CHECK(upArrow->modifiers == mods::Shift);
    CHECK_FALSE(PhysicalKeyHook::decode(0xE5, 1, mods::None)); // no scan code: ignored

    // The controller acts on it like any other press.
    Recorder r;
    KeyboardController k(r.dispatcher);
    CHECK(k.handle(*e));
    CHECK(r.log.back() == std::pair{deckCommand(0, DeckAction::Cue), ActionPhase::Press});
}

TEST_CASE("Action dispatcher: unhandled actions are ignored", "[ui][keys]")
{
    ActionDispatcher d;
    CHECK_FALSE(d.perform(globalCommand(GlobalAction::Record), ActionPhase::Press));
    int calls = 0;
    d.setHandler(globalCommand(GlobalAction::Record), [&](ActionPhase) { ++calls; });
    CHECK(d.perform(globalCommand(GlobalAction::Record), ActionPhase::Press));
    CHECK(calls == 1);
}

#if defined(_WIN32)
TEST_CASE("Hook: installs on the UI thread and removes itself", "[ui][keys]")
{
    // Windows only runs keyboard hooks for real input (not posted messages), so a test can't press keys
    // without typing into whatever window is in front; the decoding is tested above, and the manual test note
    // in DEVLOG covers real keys with the Korean IME on.
    {
        PhysicalKeyHook hook([](const KeyboardController::KeyEvent&) { return false; });
        CHECK(hook.isInstalled());
    }
    PhysicalKeyHook again([](const KeyboardController::KeyEvent&) { return false; });
    CHECK(again.isInstalled());
}
#endif
