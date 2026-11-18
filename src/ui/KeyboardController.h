#pragma once

#include "ui/Actions.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace devildj::ui
{

// Physical key positions: PC set-1 scan codes (US layout names). Extended keys (arrows, page up/down, ...)
// carry kExtended. Scan codes don't change with the input language, so Korean (or any) IME can't break them.
namespace scan
{
constexpr std::uint16_t kExtended = 0x100;
constexpr std::uint16_t Escape = 0x01, Digit1 = 0x02, Digit2 = 0x03, Digit3 = 0x04, Digit4 = 0x05, Digit5 = 0x06,
                        Digit6 = 0x07, Digit7 = 0x08, Digit8 = 0x09, Digit9 = 0x0A, Digit0 = 0x0B, Minus = 0x0C,
                        Equals = 0x0D, Tab = 0x0F, Q = 0x10, W = 0x11, E = 0x12, R = 0x13, T = 0x14, Y = 0x15, U = 0x16,
                        I = 0x17, O = 0x18, P = 0x19, Enter = 0x1C, A = 0x1E, S = 0x1F, D = 0x20, F = 0x21, G = 0x22,
                        H = 0x23, J = 0x24, K = 0x25, L = 0x26, Semicolon = 0x27, Quote = 0x28, Backquote = 0x29,
                        Z = 0x2C, X = 0x2D, C = 0x2E, V = 0x2F, B = 0x30, N = 0x31, M = 0x32, Comma = 0x33,
                        Period = 0x34, Slash = 0x35, Space = 0x39, F1 = 0x3B, F2 = 0x3C, F3 = 0x3D, F4 = 0x3E,
                        F5 = 0x3F, F6 = 0x40, F7 = 0x41, F8 = 0x42, F9 = 0x43, F10 = 0x44, F11 = 0x57, F12 = 0x58,
                        Delete = kExtended | 0x53, Up = kExtended | 0x48, Down = kExtended | 0x50,
                        Left = kExtended | 0x4B, Right = kExtended | 0x4D, PageUp = kExtended | 0x49,
                        PageDown = kExtended | 0x51;
// Modifier keys themselves (never bound; they only change the chord).
constexpr std::uint16_t LeftShift = 0x2A, RightShift = 0x36, LeftCtrl = 0x1D, RightCtrl = kExtended | 0x1D,
                        LeftAlt = 0x38, RightAlt = kExtended | 0x38, LeftWin = kExtended | 0x5B,
                        RightWin = kExtended | 0x5C;
bool isModifier(std::uint16_t code) noexcept;
const char* name(std::uint16_t code) noexcept;            // "Z", ",", "F12", "Up", ... ("?" if unknown)
std::uint16_t fromName(const std::string& name) noexcept; // inverse of name(), any case; 0 if unknown
} // namespace scan

namespace mods
{
constexpr std::uint8_t None = 0, Shift = 1, Ctrl = 2, Alt = 4;
}

// Where the keyboard focus is. Main = the performance view (decks, mixer, browser). Panel contexts take
// priority over Main while they have focus; a text field takes every key (no shortcuts at all).
enum class KeyContext : std::uint8_t
{
    Main,
    BeatMaker,
    CodeBeat,
};

struct KeyChord
{
    std::uint16_t scanCode = 0;
    std::uint8_t modifiers = mods::None;
    bool operator==(const KeyChord&) const = default;
};

struct KeyBinding
{
    KeyChord chord;
    CommandId command;
    KeyContext context = KeyContext::Main;
};

// The default map from docs/KEYMAP.md.
std::vector<KeyBinding> defaultKeymap();
// Chords bound to more than one action in the same context (the default map must have none).
std::vector<std::pair<KeyBinding, KeyBinding>> findConflicts(const std::vector<KeyBinding>& bindings);
std::string describeChord(const KeyChord& chord);            // "Ctrl+Shift+N"
std::optional<KeyChord> parseChord(const std::string& text); // "Ctrl+Shift+N", "Shift+,", "F12"; nullopt if bad
std::string contextName(KeyContext context);                 // "main", "beatmaker", "codebeat"
std::optional<KeyContext> parseContext(const std::string& text);

// User keymap (Documents\Devil-DJ\Keymaps\keymap.json): overrides on top of a base map.
//   { "version": 1, "bindings": [ { "action": "deckA.Cue", "key": "Ctrl+Shift+Z", "context": "main" },
//                                 { "action": "global.Quantize", "key": "" } ] }   ("" = no key)
// Binding an action moves it (its old keys in that context go) and takes the chord from whatever had it.
// Problems (unknown action / key / context, bad JSON) go to `errors` and that entry is skipped.
std::vector<KeyBinding> applyKeymapOverrides(std::vector<KeyBinding> base, const std::string& json,
                                             std::vector<std::string>& errors);
std::string exampleKeymapJson();

// Turns physical key events into actions with press / release / repeat semantics (P1-16):
//   - a key's release always releases the action its press started, even if the modifiers changed meanwhile
//     (Shift let go first, ...), so holds never get stuck;
//   - auto-repeat is ignored, except for actions that repeat (tempo steps, list moves);
//   - nothing fires while a text field has focus.
class KeyboardController
{
public:
    KeyboardController(ActionDispatcher& dispatcher, std::vector<KeyBinding> bindings = defaultKeymap());

    struct KeyEvent
    {
        std::uint16_t scanCode = 0;
        bool down = true;
        bool repeat = false; // the key was already down (auto-repeat)
        std::uint8_t modifiers = mods::None;
    };

    // Returns true if the event was used (or belongs to a held action): the caller should then swallow it.
    bool handle(const KeyEvent& event, KeyContext focus = KeyContext::Main, bool textInputFocused = false);

    bool isHeld(CommandId id) const; // pressed and not yet released
    void releaseAll();               // focus lost / window deactivated: release every held action
    std::optional<CommandId> lookup(const KeyChord& chord, KeyContext focus) const;
    const std::vector<KeyBinding>& currentBindings() const noexcept { return bindings; }
    void setBindings(std::vector<KeyBinding> newBindings); // releases anything held first

private:
    ActionDispatcher& dispatcher;
    std::vector<KeyBinding> bindings;
    std::unordered_map<std::uint16_t, CommandId> held; // scan code -> action its press started
};

// Windows: a keyboard hook on the UI thread reads the hardware scan code of every key message for this thread
// (the IME rewrites the virtual-key code to VK_PROCESSKEY, but not the scan code), and hands it to `onKey`
// before JUCE or the IME see it; returning true swallows the key. Other platforms: not installed.
class PhysicalKeyHook
{
public:
    using Callback = std::function<bool(const KeyboardController::KeyEvent&)>;
    explicit PhysicalKeyHook(Callback onKey);
    ~PhysicalKeyHook();
    PhysicalKeyHook(const PhysicalKeyHook&) = delete;
    PhysicalKeyHook& operator=(const PhysicalKeyHook&) = delete;
    bool isInstalled() const noexcept;

    // The hook's decoding of one Windows key message (public for tests): wParam = virtual key, lParam = flags.
    static std::optional<KeyboardController::KeyEvent> decode(std::uintptr_t wParam, std::intptr_t lParam,
                                                              std::uint8_t modifiers) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace devildj::ui
