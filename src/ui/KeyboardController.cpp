#include "KeyboardController.h"

#include <juce_core/juce_core.h>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>

namespace devildj::ui
{

// ---- scan code names ----

bool scan::isModifier(std::uint16_t code) noexcept
{
    switch (code)
    {
        case LeftShift:
        case RightShift:
        case LeftCtrl:
        case RightCtrl:
        case LeftAlt:
        case RightAlt:
        case LeftWin:
        case RightWin:
            return true;
        default:
            return false;
    }
}

const char* scan::name(std::uint16_t code) noexcept
{
    struct Name
    {
        std::uint16_t code;
        const char* text;
    };
    static constexpr Name names[] = {
        {Escape, "Esc"},
        {Digit1, "1"},
        {Digit2, "2"},
        {Digit3, "3"},
        {Digit4, "4"},
        {Digit5, "5"},
        {Digit6, "6"},
        {Digit7, "7"},
        {Digit8, "8"},
        {Digit9, "9"},
        {Digit0, "0"},
        {Minus, "-"},
        {Equals, "="},
        {Tab, "Tab"},
        {Q, "Q"},
        {W, "W"},
        {E, "E"},
        {R, "R"},
        {T, "T"},
        {Y, "Y"},
        {U, "U"},
        {I, "I"},
        {O, "O"},
        {P, "P"},
        {Enter, "Enter"},
        {A, "A"},
        {S, "S"},
        {D, "D"},
        {F, "F"},
        {G, "G"},
        {H, "H"},
        {J, "J"},
        {K, "K"},
        {L, "L"},
        {Semicolon, ";"},
        {Quote, "'"},
        {Backquote, "`"},
        {Z, "Z"},
        {X, "X"},
        {C, "C"},
        {V, "V"},
        {B, "B"},
        {N, "N"},
        {M, "M"},
        {Comma, ","},
        {Period, "."},
        {Slash, "/"},
        {Space, "Space"},
        {F1, "F1"},
        {F2, "F2"},
        {F3, "F3"},
        {F4, "F4"},
        {F5, "F5"},
        {F6, "F6"},
        {F7, "F7"},
        {F8, "F8"},
        {F9, "F9"},
        {F10, "F10"},
        {F11, "F11"},
        {F12, "F12"},
        {Delete, "Delete"},
        {Up, "Up"},
        {Down, "Down"},
        {Left, "Left"},
        {Right, "Right"},
        {PageUp, "PageUp"},
        {PageDown, "PageDown"},
    };
    for (const auto& n : names)
        if (n.code == code)
            return n.text;
    return "?";
}

std::uint16_t scan::fromName(const std::string& text) noexcept
{
    // Every code name() knows, matched without case ("pageup" = "PageUp").
    for (int code = 1; code < 0x200; ++code)
    {
        const std::string n = name(static_cast<std::uint16_t>(code));
        if (n == "?" || n.size() != text.size())
            continue;
        bool same = true;
        for (std::size_t i = 0; i < n.size() && same; ++i)
            same = std::tolower(static_cast<unsigned char>(n[i])) == std::tolower(static_cast<unsigned char>(text[i]));
        if (same)
            return static_cast<std::uint16_t>(code);
    }
    return 0;
}

std::optional<KeyChord> parseChord(const std::string& text)
{
    KeyChord chord;
    std::size_t start = 0;
    while (true)
    {
        // Modifiers are the parts before a '+' (the key itself can't contain '+').
        const auto plus = text.find('+', start);
        if (plus == std::string::npos || plus + 1 >= text.size())
            break;
        std::string part = text.substr(start, plus - start);
        for (auto& c : part)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (part == "ctrl" || part == "control")
            chord.modifiers |= mods::Ctrl;
        else if (part == "shift")
            chord.modifiers |= mods::Shift;
        else if (part == "alt")
            chord.modifiers |= mods::Alt;
        else
            return std::nullopt;
        start = plus + 1;
    }
    chord.scanCode = scan::fromName(text.substr(start));
    if (chord.scanCode == 0)
        return std::nullopt;
    return chord;
}

std::string contextName(KeyContext context)
{
    switch (context)
    {
        case KeyContext::BeatMaker:
            return "beatmaker";
        case KeyContext::CodeBeat:
            return "codebeat";
        case KeyContext::Main:
        default:
            return "main";
    }
}

std::optional<KeyContext> parseContext(const std::string& text)
{
    for (auto c : {KeyContext::Main, KeyContext::BeatMaker, KeyContext::CodeBeat})
        if (contextName(c) == text)
            return c;
    return std::nullopt;
}

std::vector<KeyBinding> applyKeymapOverrides(std::vector<KeyBinding> base, const std::string& json,
                                             std::vector<std::string>& errors)
{
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8(json.c_str()));
    const auto* list = parsed.getProperty("bindings", juce::var()).getArray();
    if (list == nullptr)
    {
        errors.push_back(json.empty() ? "empty file" : "no \"bindings\" list (or not valid JSON)");
        return base;
    }
    for (const auto& entry : *list)
    {
        const std::string action = entry.getProperty("action", "").toString().toStdString();
        const std::string key = entry.getProperty("key", "").toString().toStdString();
        const std::string contextText = entry.getProperty("context", "main").toString().toStdString();
        const auto command = commandFromKey(action);
        const auto context = parseContext(contextText);
        if (!command)
        {
            errors.push_back("unknown action \"" + action + "\"");
            continue;
        }
        if (!context)
        {
            errors.push_back("unknown context \"" + contextText + "\" for " + action);
            continue;
        }
        std::optional<KeyChord> chord;
        if (!key.empty())
        {
            chord = parseChord(key);
            if (!chord || scan::isModifier(chord->scanCode))
            {
                errors.push_back("unknown key \"" + key + "\" for " + action);
                continue;
            }
        }
        // The action moves: drop its old keys in this context, and whatever held the new chord.
        std::erase_if(base, [&](const KeyBinding& b)
                      { return b.context == *context && (b.command == *command || (chord && b.chord == *chord)); });
        if (chord)
            base.push_back({*chord, *command, *context});
    }
    return base;
}

std::string exampleKeymapJson()
{
    return R"({
  "version": 1,
  "comment": "Devil-DJ keymap. Each entry moves one action to a new key (\"\" = no key). Keys: Z, Shift+B, Ctrl+Shift+N, F5, Up, ... (US key positions). Action names: deckA.Cue, deckB.Play, global.CrossfaderCentre, ... (Hold Ctrl+/ in the app to see the current map.) After editing: Options > Reload keymap.",
  "bindings": [
    { "action": "deckA.Cue", "key": "Z" },
    { "action": "global.CrossfaderCentre", "key": "B" }
  ]
}
)";
}

std::string describeChord(const KeyChord& chord)
{
    std::string s;
    if (chord.modifiers & mods::Ctrl)
        s += "Ctrl+";
    if (chord.modifiers & mods::Alt)
        s += "Alt+";
    if (chord.modifiers & mods::Shift)
        s += "Shift+";
    return s + scan::name(chord.scanCode);
}

// ---- default keymap (docs/KEYMAP.md) ----

std::vector<KeyBinding> defaultKeymap()
{
    using namespace scan;
    using DA = DeckAction;
    using GA = GlobalAction;
    std::vector<KeyBinding> map;
    const auto deckKey = [&map](int deck, std::uint16_t code, std::uint8_t m, DA action)
    { map.push_back({{code, m}, deckCommand(deck, action), KeyContext::Main}); };
    const auto key = [&map](std::uint16_t code, std::uint8_t m, GA action, KeyContext context = KeyContext::Main)
    { map.push_back({{code, m}, globalCommand(action), context}); };

    // Decks: left hand A, right hand B. {A key, B key} per action.
    struct DeckRow
    {
        std::uint16_t a, b;
        std::uint8_t mods;
        DA action;
    };
    const DeckRow rows[] = {
        {Z, M, mods::None, DA::Cue},
        {X, Comma, mods::None, DA::Play},
        {C, Period, mods::None, DA::Sync},
        {V, Slash, mods::None, DA::AutoLoop},
        {Q, U, mods::None, DA::Pad1},
        {W, I, mods::None, DA::Pad2},
        {E, O, mods::None, DA::Pad3},
        {R, P, mods::None, DA::Pad4},
        {A, J, mods::None, DA::Pad5},
        {S, K, mods::None, DA::Pad6},
        {D, L, mods::None, DA::Pad7},
        {F, Semicolon, mods::None, DA::Pad8},
        {Q, U, mods::Shift, DA::PadAlt1},
        {W, I, mods::Shift, DA::PadAlt2},
        {E, O, mods::Shift, DA::PadAlt3},
        {R, P, mods::Shift, DA::PadAlt4},
        {A, J, mods::Shift, DA::PadAlt5},
        {S, K, mods::Shift, DA::PadAlt6},
        {D, L, mods::Shift, DA::PadAlt7},
        {F, Semicolon, mods::Shift, DA::PadAlt8},
        {Digit1, Digit7, mods::None, DA::BendDown},
        {Digit2, Digit8, mods::None, DA::BendUp},
        {Digit3, Digit9, mods::None, DA::LoopHalve},
        {Digit4, Digit0, mods::None, DA::LoopDouble},
        {Digit5, Minus, mods::None, DA::BeatJumpBack},
        {Digit6, Equals, mods::None, DA::BeatJumpForward},
        {Digit5, Minus, mods::Ctrl, DA::FineJumpBack}, // P2-21 (Ctrl+Shift+5 is a rating: the size is on 6 / =)
        {Digit6, Equals, mods::Ctrl, DA::FineJumpForward},
        {Digit6, Equals, mods::Shift, DA::BeatJumpSizeUp},
        {Digit6, Equals, mods::Ctrl | mods::Shift, DA::BeatJumpSizeDown},
        {Digit1, Digit7, mods::Shift, DA::TempoDown},
        {Digit2, Digit8, mods::Shift, DA::TempoUp},
        {Digit3, Digit9, mods::Shift, DA::FilterToLowPass},
        {Digit4, Digit0, mods::Shift, DA::FilterToHighPass},
        {Digit5, Minus, mods::Shift, DA::FilterReset},
        {Z, M, mods::Shift, DA::JumpToStart},
        {X, Comma, mods::Shift, DA::Reverse},
        {C, Period, mods::Shift, DA::KeySync},
        {V, Slash, mods::Shift, DA::Reloop},
        {A, J, mods::Ctrl, DA::KillLow},
        {S, K, mods::Ctrl, DA::KillMid},
        {D, L, mods::Ctrl, DA::KillHigh},
        {Q, U, mods::Ctrl, DA::Slip},
        {W, I, mods::Ctrl, DA::KeyLock},
        {E, O, mods::Ctrl, DA::HeadphoneCue},
        {R, P, mods::Ctrl, DA::MakeMaster},
        // Beatgrid (P2-14): Ctrl+Shift on the deck's keys.
        {Z, M, mods::Ctrl | mods::Shift, DA::GridDownbeat},
        {A, J, mods::Ctrl | mods::Shift, DA::GridEarlier},
        {S, K, mods::Ctrl | mods::Shift, DA::GridLater},
        {Q, U, mods::Ctrl | mods::Shift, DA::GridBeatBack},
        {W, I, mods::Ctrl | mods::Shift, DA::GridBeatForward},
        {E, O, mods::Ctrl | mods::Shift, DA::GridDouble},
        {R, P, mods::Ctrl | mods::Shift, DA::GridHalve},
        {X, Comma, mods::Ctrl | mods::Shift, DA::TapTempo},
        {C, Period, mods::Ctrl | mods::Shift, DA::Metronome},
        {V, Slash, mods::Ctrl | mods::Shift, DA::GridLock},
    };
    for (const auto& r : rows)
    {
        deckKey(0, r.a, r.mods, r.action);
        deckKey(1, r.b, r.mods, r.action);
    }
    // Loading (browser keys; for now they open the file chooser): Tab / Enter = A / B, with Shift = C / D.
    deckKey(0, Tab, mods::None, DA::Load);
    deckKey(1, Enter, mods::None, DA::Load);
    deckKey(2, Tab, mods::Shift, DA::Load);
    deckKey(3, Enter, mods::Shift, DA::Load);

    // Mixer, FX and global.
    key(G, mods::None, GA::CrossfaderLeft);
    key(H, mods::None, GA::CrossfaderRight);
    key(G, mods::Shift, GA::CrossfaderSnapLeft);
    key(H, mods::Shift, GA::CrossfaderSnapRight);
    key(B, mods::None, GA::CrossfaderCentre);
    key(T, mods::None, GA::AutoTransition);
    key(T, mods::Shift, GA::AutoTransitionCancel);
    key(N, mods::None, GA::LoadSuggestion);
    key(N, mods::Shift, GA::NextSuggestion);
    key(N, mods::Ctrl, GA::PreviewSuggestion);
    key(N, mods::Ctrl | mods::Shift, GA::CycleIntent);
    key(Quote, mods::None, GA::RateTransitionUp);
    key(Quote, mods::Shift, GA::RateTransitionDown);
    key(Backquote, mods::None, GA::Quantize);
    key(F1, mods::None, GA::PadModeHotCue);
    key(F2, mods::None, GA::PadModeBeatLoop);
    key(F3, mods::None, GA::PadModeBeatJump);
    key(F4, mods::None, GA::PadModeSlicer);
    key(F1, mods::Shift, GA::PadModeKeyShift);
    key(F2, mods::Shift, GA::PadModeKeyboard);
    key(F3, mods::Shift, GA::PadModePadFx);
    key(F4, mods::Shift, GA::PadModeSampler);
    key(F5, mods::None, GA::Fx1Toggle);
    key(F6, mods::None, GA::Fx1Next);
    key(F7, mods::None, GA::Fx2Toggle);
    key(F8, mods::None, GA::Fx2Next);
    key(F5, mods::Shift, GA::Fx1Target);
    key(F7, mods::Shift, GA::Fx2Target);
    key(F9, mods::None, GA::Record);
    key(F10, mods::None, GA::Automix);
    key(F11, mods::None, GA::FullScreen);
    key(F12, mods::None, GA::DebugOverlay);
    key(Escape, mods::None, GA::Panic);
    key(Slash, mods::Ctrl, GA::KeymapOverlay);

    // Browser (part of the main view).
    key(Up, mods::None, GA::BrowserUp);
    key(Down, mods::None, GA::BrowserDown);
    key(PageUp, mods::None, GA::BrowserPageUp);
    key(PageDown, mods::None, GA::BrowserPageDown);
    key(Left, mods::None, GA::BrowserCollapse);
    key(Right, mods::None, GA::BrowserExpand);
    key(F, mods::Ctrl, GA::BrowserSearch);
    key(B, mods::Ctrl, GA::BrowserBigger);
    key(Delete, mods::None, GA::BrowserRemove);
    const std::uint16_t digits[] = {Digit1, Digit2, Digit3, Digit4, Digit5};
    for (int i = 0; i < 5; ++i)
        key(digits[i], mods::Ctrl | mods::Shift, static_cast<GA>(static_cast<int>(GA::Rate1) + i));
    key(Digit0, mods::Ctrl | mods::Shift, GA::RateClear);
    key(Space, mods::Ctrl, GA::PreviewTrack);
    key(Tab, mods::Ctrl, GA::SwitchPanel);

    // Beat Maker panel.
    constexpr auto bm = KeyContext::BeatMaker;
    key(Space, mods::None, GA::BeatMakerStartStop, bm);
    key(Up, mods::None, GA::BeatMakerCursorUp, bm);
    key(Down, mods::None, GA::BeatMakerCursorDown, bm);
    key(Left, mods::None, GA::BeatMakerCursorLeft, bm);
    key(Right, mods::None, GA::BeatMakerCursorRight, bm);
    key(Enter, mods::None, GA::BeatMakerToggleStep, bm);
    key(Down, mods::Shift, GA::BeatMakerVelocityDown, bm);
    key(Up, mods::Shift, GA::BeatMakerVelocityUp, bm);
    const std::uint16_t eight[] = {Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8};
    for (int i = 0; i < 8; ++i)
    {
        key(eight[i], mods::None, static_cast<GA>(static_cast<int>(GA::BeatMakerTrack1) + i), bm);
        key(eight[i], mods::Ctrl, static_cast<GA>(static_cast<int>(GA::BeatMakerPattern1) + i), bm);
    }
    key(C, mods::Ctrl, GA::BeatMakerCopy, bm);
    key(V, mods::Ctrl, GA::BeatMakerPaste, bm);
    key(Delete, mods::None, GA::BeatMakerClear, bm);
    key(Z, mods::Ctrl, GA::BeatMakerUndo, bm);
    key(Y, mods::Ctrl, GA::BeatMakerRedo, bm);

    // Code Beat editor.
    constexpr auto cb = KeyContext::CodeBeat;
    key(Enter, mods::Ctrl, GA::CodeEvaluate, cb);
    key(Enter, mods::Ctrl | mods::Shift, GA::CodeEvaluateNow, cb);
    key(Period, mods::Ctrl, GA::CodeHush, cb);
    key(Backquote, mods::Ctrl, GA::CodeConsole, cb);
    key(PageDown, mods::Ctrl, GA::CodeNextBuffer, cb);
    key(PageUp, mods::Ctrl, GA::CodePreviousBuffer, cb);
    key(Space, mods::Ctrl, GA::CodeAutocomplete, cb);
    return map;
}

std::vector<std::pair<KeyBinding, KeyBinding>> findConflicts(const std::vector<KeyBinding>& bindings)
{
    std::vector<std::pair<KeyBinding, KeyBinding>> conflicts;
    for (std::size_t i = 0; i < bindings.size(); ++i)
        for (std::size_t j = i + 1; j < bindings.size(); ++j)
            if (bindings[i].context == bindings[j].context && bindings[i].chord == bindings[j].chord)
                conflicts.emplace_back(bindings[i], bindings[j]);
    return conflicts;
}

// ---- controller ----

KeyboardController::KeyboardController(ActionDispatcher& d, std::vector<KeyBinding> b)
    : dispatcher(d)
    , bindings(std::move(b))
{
}

std::optional<CommandId> KeyboardController::lookup(const KeyChord& chord, KeyContext focus) const
{
    // The focused panel's own bindings first, then the main view's.
    for (const auto context : {focus, KeyContext::Main})
    {
        for (const auto& b : bindings)
            if (b.context == context && b.chord == chord)
                return b.command;
        if (focus == KeyContext::Main)
            break;
    }
    return std::nullopt;
}

bool KeyboardController::handle(const KeyEvent& e, KeyContext focus, bool textInputFocused)
{
    if (scan::isModifier(e.scanCode))
        return false;

    if (!e.down)
    {
        // A release ends whatever this key's press started, whatever the modifiers are now.
        const auto it = held.find(e.scanCode);
        if (it == held.end())
            return false;
        const auto command = it->second;
        held.erase(it);
        dispatcher.perform(command, ActionPhase::Release);
        return true;
    }

    if (e.repeat)
    {
        const auto it = held.find(e.scanCode);
        if (it == held.end())
            return false; // a repeat of a key we never took (e.g. typed in a text field)
        if (commandRepeats(it->second))
            dispatcher.perform(it->second, ActionPhase::Repeat);
        return true; // swallow other repeats: holds must not re-trigger
    }

    if (textInputFocused)
        return false;
    const auto command = lookup({e.scanCode, e.modifiers}, focus);
    if (!command)
        return false;
    if (const auto it = held.find(e.scanCode); it != held.end()) // missed release (focus change): end it first
        dispatcher.perform(it->second, ActionPhase::Release);
    held[e.scanCode] = *command;
    dispatcher.perform(*command, ActionPhase::Press);
    return true;
}

void KeyboardController::setBindings(std::vector<KeyBinding> newBindings)
{
    releaseAll();
    bindings = std::move(newBindings);
}

bool KeyboardController::isHeld(CommandId id) const
{
    return std::any_of(held.begin(), held.end(), [id](const auto& h) { return h.second == id; });
}

void KeyboardController::releaseAll()
{
    auto pending = std::move(held);
    held.clear();
    for (const auto& [code, command] : pending)
        dispatcher.perform(command, ActionPhase::Release);
}

// ---- Windows hook ----

std::optional<KeyboardController::KeyEvent> PhysicalKeyHook::decode(std::uintptr_t /*wParam*/, std::intptr_t lParam,
                                                                    std::uint8_t modifiers) noexcept
{
    // KeyboardProc lParam: bits 16-23 scan code, 24 extended, 30 previous key state, 31 transition (1 = up).
    const auto flags = static_cast<std::uint32_t>(lParam);
    std::uint16_t code = static_cast<std::uint16_t>((flags >> 16) & 0xFF);
    if (code == 0)
        return std::nullopt; // synthetic keys without a scan code (e.g. some IME / injected events)
    if (flags & (1u << 24))
        code |= scan::kExtended;
    KeyboardController::KeyEvent e;
    e.scanCode = code;
    e.down = (flags & (1u << 31)) == 0;
    e.repeat = e.down && (flags & (1u << 30)) != 0;
    e.modifiers = modifiers;
    return e;
}

#if JUCE_WINDOWS
struct PhysicalKeyHook::Impl
{
    Callback onKey;
    HHOOK hook = nullptr;
    static inline Impl* active = nullptr; // one per UI thread is enough (the app has one)

    static std::uint8_t currentModifiers() noexcept
    {
        std::uint8_t m = mods::None;
        if (GetKeyState(VK_SHIFT) & 0x8000)
            m |= mods::Shift;
        if (GetKeyState(VK_CONTROL) & 0x8000)
            m |= mods::Ctrl;
        if (GetKeyState(VK_MENU) & 0x8000)
            m |= mods::Alt;
        return m;
    }

    static LRESULT CALLBACK proc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HC_ACTION && active != nullptr && active->onKey)
            if (const auto e =
                    decode(static_cast<std::uintptr_t>(wParam), static_cast<std::intptr_t>(lParam), currentModifiers()))
                if (active->onKey(*e))
                    return 1; // used: neither the IME nor JUCE sees it
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
};

PhysicalKeyHook::PhysicalKeyHook(Callback onKey)
    : impl(std::make_unique<Impl>())
{
    impl->onKey = std::move(onKey);
    impl->hook = SetWindowsHookExW(WH_KEYBOARD, &Impl::proc, nullptr, GetCurrentThreadId());
    if (impl->hook != nullptr)
        Impl::active = impl.get();
}

PhysicalKeyHook::~PhysicalKeyHook()
{
    if (impl->hook != nullptr)
        UnhookWindowsHookEx(impl->hook);
    if (Impl::active == impl.get())
        Impl::active = nullptr;
}

bool PhysicalKeyHook::isInstalled() const noexcept
{
    return impl->hook != nullptr;
}
#else
struct PhysicalKeyHook::Impl
{
};
PhysicalKeyHook::PhysicalKeyHook(Callback)
    : impl(std::make_unique<Impl>())
{
}
PhysicalKeyHook::~PhysicalKeyHook() = default;
bool PhysicalKeyHook::isInstalled() const noexcept
{
    return false;
}
#endif

} // namespace devildj::ui
