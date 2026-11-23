#pragma once

// Every bindable user action (CLAUDE.md: "Every action is a CommandId in ui/Actions.h so the keymap and MIDI
// Learn can bind it"). Deck actions exist once per deck (A-D); the rest are global or belong to a panel.
// Actions whose feature isn't built yet are listed too; with no handler registered they do nothing.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace devildj::ui
{

#define DEVILDJ_DECK_ACTIONS(X)                                                                                        \
    X(Cue, "Cue (hold to preview)", false)                                                                             \
    X(Play, "Play / pause", false)                                                                                     \
    X(Sync, "Sync on/off", false)                                                                                      \
    X(AutoLoop, "Auto loop 4 beats", false)                                                                            \
    X(Pad1, "Pad 1", false)                                                                                            \
    X(Pad2, "Pad 2", false)                                                                                            \
    X(Pad3, "Pad 3", false)                                                                                            \
    X(Pad4, "Pad 4", false)                                                                                            \
    X(Pad5, "Pad 5", false)                                                                                            \
    X(Pad6, "Pad 6", false)                                                                                            \
    X(Pad7, "Pad 7", false)                                                                                            \
    X(Pad8, "Pad 8", false)                                                                                            \
    X(PadAlt1, "Pad 1 alternate", false)                                                                               \
    X(PadAlt2, "Pad 2 alternate", false)                                                                               \
    X(PadAlt3, "Pad 3 alternate", false)                                                                               \
    X(PadAlt4, "Pad 4 alternate", false)                                                                               \
    X(PadAlt5, "Pad 5 alternate", false)                                                                               \
    X(PadAlt6, "Pad 6 alternate", false)                                                                               \
    X(PadAlt7, "Pad 7 alternate", false)                                                                               \
    X(PadAlt8, "Pad 8 alternate", false)                                                                               \
    X(BendDown, "Pitch bend - (hold)", false)                                                                          \
    X(BendUp, "Pitch bend + (hold)", false)                                                                            \
    X(LoopHalve, "Loop halve", false)                                                                                  \
    X(LoopDouble, "Loop double", false)                                                                                \
    X(BeatJumpBack, "Beat jump back", false)                                                                           \
    X(BeatJumpForward, "Beat jump forward", false)                                                                     \
    X(TempoDown, "Tempo -0.1 %", true)                                                                                 \
    X(TempoUp, "Tempo +0.1 %", true)                                                                                   \
    X(FilterToLowPass, "Filter sweep to low-pass (hold)", false)                                                       \
    X(FilterToHighPass, "Filter sweep to high-pass (hold)", false)                                                     \
    X(FilterReset, "Filter reset", false)                                                                              \
    X(JumpToStart, "Jump to track start", false)                                                                       \
    X(Reverse, "Reverse (hold)", false)                                                                                \
    X(KeySync, "Key Sync", false)                                                                                      \
    X(Reloop, "Reloop / loop exit", false)                                                                             \
    X(KillLow, "Low EQ kill", false)                                                                                   \
    X(KillMid, "Mid EQ kill", false)                                                                                   \
    X(KillHigh, "High EQ kill", false)                                                                                 \
    X(Slip, "Slip mode", false)                                                                                        \
    X(KeyLock, "Key lock", false)                                                                                      \
    X(HeadphoneCue, "Headphone cue", false)                                                                            \
    X(MakeMaster, "Make master deck", false)                                                                           \
    X(Load, "Load track", false)                                                                                       \
    X(GridDownbeat, "Grid: set downbeat here", false)                                                                  \
    X(GridEarlier, "Grid: 1 ms earlier", true)                                                                         \
    X(GridLater, "Grid: 1 ms later", true)                                                                             \
    X(GridBeatBack, "Grid: downbeat one beat earlier", false)                                                          \
    X(GridBeatForward, "Grid: downbeat one beat later", false)                                                         \
    X(GridDouble, "Grid: BPM x2", false)                                                                               \
    X(GridHalve, "Grid: BPM /2", false)                                                                                \
    X(TapTempo, "Tap tempo", false)                                                                                    \
    X(Metronome, "Metronome on / off (headphones)", false)                                                             \
    X(GridLock, "Grid: lock / unlock (analysis leaves it alone)", false)                                              \
    X(BeatJumpSizeDown, "Beat jump size smaller", false)                                                               \
    X(BeatJumpSizeUp, "Beat jump size larger", false)                                                                  \
    X(FineJumpBack, "Fine jump back 5 ms", true)                                                                       \
    X(FineJumpForward, "Fine jump forward 5 ms", true)                                                                 \
    X(KeyShiftDown, "Key shift -1 semitone", false)                                                                    \
    X(KeyShiftUp, "Key shift +1 semitone", false)                                                                      \
    X(MemoryAdd, "Store a memory cue here", false)                                                                     \
    X(CallPrevious, "Call the previous memory cue", false)                                                             \
    X(CallNext, "Call the next memory cue", false)                                                                     \
    X(LoopIn, "Loop in", false)                                                                                        \
    X(LoopOut, "Loop out", false)                                                                                      \
    X(Brake, "Vinyl brake (stop)", false)                                                                              \
    X(SpinBack, "Spin-back (stop)", false)                                                                                 X(ReleaseEchoOut, "Release FX: echo out (hold)", false)                                                                X(ReleaseBrake, "Release FX: brake (hold)", false)                                                                     X(ReleaseSpinBack, "Release FX: spin back (hold)", false)                                                              X(ReleaseSweep, "Release FX: filter sweep (hold)", false)                                                          \
    X(CaptureToSampler, "Capture the loop (or slicer range) to the sampler", false)

#define DEVILDJ_GLOBAL_ACTIONS(X)                                                                                      \
    X(CrossfaderLeft, "Crossfader left (hold to glide)", false)                                                        \
    X(CrossfaderRight, "Crossfader right (hold to glide)", false)                                                      \
    X(CrossfaderSnapLeft, "Crossfader fully left", false)                                                              \
    X(CrossfaderSnapRight, "Crossfader fully right", false)                                                            \
    X(CrossfaderCentre, "Crossfader to centre", false)                                                                 \
    X(AutoTransition, "Auto-transition", false)                                                                        \
    X(AutoTransitionCancel, "Cancel auto-transition", false)                                                           \
    X(LoadSuggestion, "Load highlighted suggestion", false)                                                            \
    X(NextSuggestion, "Next suggestion", true)                                                                         \
    X(PreviewSuggestion, "Preview suggestion (hold)", false)                                                           \
    X(CycleIntent, "Cycle intent", false)                                                                              \
    X(MatcherPreset, "Next Track: next preset", false)                                                                 \
    X(MatcherKeyPolicy, "Next Track: key policy (strict / relaxed / any)", false)                                      \
    X(MatcherScope, "Next Track: scope (library / the playlist shown)", false)                                         \
    X(RateTransitionUp, "Rate last transition up", false)                                                              \
    X(RateTransitionDown, "Rate last transition down", false)                                                          \
    X(Quantize, "Quantize on/off", false)                                                                              \
    X(PadModeHotCue, "Pad mode: Hot Cue", false)                                                                       \
    X(PadModeBeatLoop, "Pad mode: Beat Loop", false)                                                                   \
    X(PadModeBeatJump, "Pad mode: Beat Jump", false)                                                                   \
    X(PadModeSlicer, "Pad mode: Slicer", false)                                                                        \
    X(PadModeKeyShift, "Pad mode: Key Shift", false)                                                                   \
    X(PadModeKeyboard, "Pad mode: Keyboard", false)                                                                    \
    X(PadModePadFx, "Pad mode: Pad FX", false)                                                                         \
    X(PadModeSampler, "Pad mode: Sampler", false)                                                                      \
    X(Fx1Toggle, "FX1 on/off", false)                                                                                  \
    X(Fx1Next, "FX1 next effect", false)                                                                               \
    X(Fx2Toggle, "FX2 on/off", false)                                                                                  \
    X(Fx2Next, "FX2 next effect", false)                                                                               \
    X(Fx1Target, "FX1 target", false)                                                                                  \
    X(Fx2Target, "FX2 target", false)                                                                                  \
    X(Record, "Record start/stop", false)                                                                              \
    X(Automix, "Automix on/off", false)                                                                                \
    X(FullScreen, "Full screen", false)                                                                                \
    X(DebugOverlay, "Debug overlay", false)                                                                            \
    X(Panic, "Panic: release everything held", false)                                                                  \
    X(KeymapOverlay, "Show keymap (hold)", false)                                                                      \
    X(BrowserUp, "Browser: up", true)                                                                                  \
    X(BrowserDown, "Browser: down", true)                                                                              \
    X(BrowserPageUp, "Browser: page up", true)                                                                         \
    X(BrowserPageDown, "Browser: page down", true)                                                                     \
    X(BrowserCollapse, "Browser: collapse", false)                                                                     \
    X(BrowserExpand, "Browser: expand", false)                                                                         \
    X(BrowserSearch, "Browser: search", false)                                                                         \
    X(Rate1, "Rate 1 star", false)                                                                                     \
    X(Rate2, "Rate 2 stars", false)                                                                                    \
    X(Rate3, "Rate 3 stars", false)                                                                                    \
    X(Rate4, "Rate 4 stars", false)                                                                                    \
    X(Rate5, "Rate 5 stars", false)                                                                                    \
    X(RateClear, "Clear rating", false)                                                                                \
    X(PreviewTrack, "Preview selected track (hold)", false)                                                            \
    X(SwitchPanel, "Switch bottom panel", false)                                                                       \
    X(BeatMakerStartStop, "Beat Maker: start / stop", false)                                                           \
    X(BeatMakerCursorUp, "Beat Maker: cursor up", true)                                                                \
    X(BeatMakerCursorDown, "Beat Maker: cursor down", true)                                                            \
    X(BeatMakerCursorLeft, "Beat Maker: cursor left", true)                                                            \
    X(BeatMakerCursorRight, "Beat Maker: cursor right", true)                                                          \
    X(BeatMakerToggleStep, "Beat Maker: toggle step", false)                                                           \
    X(BeatMakerVelocityDown, "Beat Maker: velocity -", true)                                                           \
    X(BeatMakerVelocityUp, "Beat Maker: velocity +", true)                                                             \
    X(BeatMakerTrack1, "Beat Maker: track 1", false)                                                                   \
    X(BeatMakerTrack2, "Beat Maker: track 2", false)                                                                   \
    X(BeatMakerTrack3, "Beat Maker: track 3", false)                                                                   \
    X(BeatMakerTrack4, "Beat Maker: track 4", false)                                                                   \
    X(BeatMakerTrack5, "Beat Maker: track 5", false)                                                                   \
    X(BeatMakerTrack6, "Beat Maker: track 6", false)                                                                   \
    X(BeatMakerTrack7, "Beat Maker: track 7", false)                                                                   \
    X(BeatMakerTrack8, "Beat Maker: track 8", false)                                                                   \
    X(BeatMakerPattern1, "Beat Maker: pattern A", false)                                                               \
    X(BeatMakerPattern2, "Beat Maker: pattern B", false)                                                               \
    X(BeatMakerPattern3, "Beat Maker: pattern C", false)                                                               \
    X(BeatMakerPattern4, "Beat Maker: pattern D", false)                                                               \
    X(BeatMakerPattern5, "Beat Maker: pattern E", false)                                                               \
    X(BeatMakerPattern6, "Beat Maker: pattern F", false)                                                               \
    X(BeatMakerPattern7, "Beat Maker: pattern G", false)                                                               \
    X(BeatMakerPattern8, "Beat Maker: pattern H", false)                                                               \
    X(BeatMakerCopy, "Beat Maker: copy", false)                                                                        \
    X(BeatMakerPaste, "Beat Maker: paste", false)                                                                      \
    X(BeatMakerClear, "Beat Maker: clear steps", false)                                                                \
    X(BeatMakerUndo, "Beat Maker: undo", false)                                                                        \
    X(BeatMakerRedo, "Beat Maker: redo", false)                                                                        \
    X(BeatMakerProbabilityDown, "Beat Maker: step probability -", true)                                                \
    X(BeatMakerProbabilityUp, "Beat Maker: step probability +", true)                                                  \
    X(BeatMakerMicroEarlier, "Beat Maker: step earlier (micro-timing)", true)                                          \
    X(BeatMakerMicroLater, "Beat Maker: step later (micro-timing)", true)                                              \
    X(BeatMakerRatchet, "Beat Maker: step ratchet 1-4", false)                                                         \
    X(BeatMakerStepEditor, "Beat Maker: step editor", false)                                                           \
    X(BeatMakerMute, "Beat Maker: mute track", false)                                                                  \
    X(BeatMakerSolo, "Beat Maker: solo track", false)                                                                  \
    X(BeatMakerSoundPrevious, "Beat Maker: previous sound", true)                                                      \
    X(BeatMakerSoundNext, "Beat Maker: next sound", true)                                                              \
    X(BeatMakerLength, "Beat Maker: pattern length 16 / 32 / 64", false)                                               \
    X(BeatMakerPagePrevious, "Beat Maker: previous page", false)                                                       \
    X(BeatMakerPageNext, "Beat Maker: next page", false)                                                               \
    X(BeatMakerSwingDown, "Beat Maker: swing -", true)                                                                 \
    X(BeatMakerSwingUp, "Beat Maker: swing +", true)                                                                   \
    X(BeatMakerNoteLeft, "Beat Maker: move note earlier", true)                                                        \
    X(BeatMakerNoteRight, "Beat Maker: move note later", true)                                                         \
    X(BeatMakerNoteUp, "Beat Maker: move note up", true)                                                               \
    X(BeatMakerNoteDown, "Beat Maker: move note down", true)                                                           \
    X(BeatMakerQuantize, "Beat Maker: quantize notes", false)                                                          \
    X(BeatMakerChord, "Beat Maker: chord stamp", false)                                                                \
    X(BeatMakerScale, "Beat Maker: scale", false)                                                                      \
    X(BeatMakerScaleSnap, "Beat Maker: snap to scale", false)                                                          \
    X(BeatMakerSnap, "Beat Maker: note snap", false)                                                                   \
    X(BeatMakerArp, "Beat Maker: arpeggiator on / off", false)                                                         \
    X(BeatMakerArpRate, "Beat Maker: arpeggiator rate", false)                                                         \
    X(BeatMakerArpMode, "Beat Maker: arpeggiator mode", false)                                                         \
    X(BeatMakerArpOctaves, "Beat Maker: arpeggiator octaves", false)                                                   \
    X(BeatMakerView, "Beat Maker: drums / synth view", false)                                                          \
    X(CodeEvaluate, "Code Beat: evaluate", false)                                                                      \
    X(CodeEvaluateNow, "Code Beat: evaluate immediately", false)                                                       \
    X(CodeHush, "Code Beat: hush", false)                                                                              \
    X(CodeConsole, "Code Beat: console", false)                                                                        \
    X(CodeNextBuffer, "Code Beat: next buffer", false)                                                                 \
    X(CodePreviousBuffer, "Code Beat: previous buffer", false)                                                         \
    X(CodeAutocomplete, "Code Beat: autocomplete", false)                                                              \
    X(BrowserBigger, "Browser: bigger / smaller", false)                                                               \
    X(BrowserRemove, "Browser: remove from playlist", false)

enum class DeckAction : std::uint16_t
{
#define DEVILDJ_ENUM(name, text, repeats) name,
    DEVILDJ_DECK_ACTIONS(DEVILDJ_ENUM)
#undef DEVILDJ_ENUM
    Count
};

enum class GlobalAction : std::uint16_t
{
#define DEVILDJ_ENUM(name, text, repeats) name,
    DEVILDJ_GLOBAL_ACTIONS(DEVILDJ_ENUM)
#undef DEVILDJ_ENUM
    Count
};

constexpr int kCommandDecks = 4;
constexpr int kDeckActionCount = static_cast<int>(DeckAction::Count);
constexpr int kGlobalActionCount = static_cast<int>(GlobalAction::Count);
constexpr int kCommandCount = kCommandDecks * kDeckActionCount + kGlobalActionCount;

// A bindable action. Values: deck actions first (deck * kDeckActionCount + action), then the global ones.
// The numbers aren't stable across versions; saved keymaps use commandKey() names.
enum class CommandId : std::uint16_t
{
};

constexpr CommandId deckCommand(int deck, DeckAction a) noexcept
{
    return static_cast<CommandId>(deck * kDeckActionCount + static_cast<int>(a));
}
constexpr CommandId globalCommand(GlobalAction a) noexcept
{
    return static_cast<CommandId>(kCommandDecks * kDeckActionCount + static_cast<int>(a));
}
constexpr bool isDeckCommand(CommandId id) noexcept
{
    return static_cast<int>(id) < kCommandDecks * kDeckActionCount;
}
constexpr int commandDeck(CommandId id) noexcept // -1 for global actions
{
    return isDeckCommand(id) ? static_cast<int>(id) / kDeckActionCount : -1;
}
constexpr DeckAction commandDeckAction(CommandId id) noexcept
{
    return static_cast<DeckAction>(static_cast<int>(id) % kDeckActionCount);
}

std::string commandName(CommandId id);      // "Deck A: Cue (hold to preview)", "Crossfader to centre"
std::string commandKey(CommandId id);       // stable id for files: "deckA.Cue", "global.CrossfaderCentre"
bool commandRepeats(CommandId id);          // fires again while the key is held (tempo steps, list moves)
std::string commandShortName(CommandId id); // fits on a keycap: "Cue", "Bend -", "Kill LO", "XF <"
std::optional<CommandId> commandFromKey(const std::string& key); // inverse of commandKey()
std::vector<CommandId> allCommands();

// How an action is triggered: a key going down, the same key coming up, or auto-repeat while held.
enum class ActionPhase : std::uint8_t
{
    Press,
    Release,
    Repeat,
};

// ARCHITECTURE §9: all user actions go through perform(); components, the KeyboardController, MIDI Learn and
// the phone remote all call it. Handlers are registered by whoever owns the feature. Message thread only.
class ActionDispatcher
{
public:
    using Handler = std::function<void(ActionPhase)>;

    void setHandler(CommandId id, Handler handler) { handlers[static_cast<std::uint16_t>(id)] = std::move(handler); }
    bool hasHandler(CommandId id) const { return handlers.count(static_cast<std::uint16_t>(id)) > 0; }

    // Returns false if nothing handles the action (yet).
    bool perform(CommandId id, ActionPhase phase) const
    {
        const auto it = handlers.find(static_cast<std::uint16_t>(id));
        if (it == handlers.end() || !it->second)
            return false;
        it->second(phase);
        return true;
    }

private:
    std::unordered_map<std::uint16_t, Handler> handlers;
};

} // namespace devildj::ui
