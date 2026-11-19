#include "Actions.h"

#include <array>

namespace devildj::ui
{

namespace
{
struct Info
{
    const char* key;
    const char* text;
    bool repeats;
};

constexpr std::array<Info, kDeckActionCount> kDeckInfo{{
#define DEVILDJ_INFO(name, text, repeats) {#name, text, repeats},
    DEVILDJ_DECK_ACTIONS(DEVILDJ_INFO)
#undef DEVILDJ_INFO
}};

constexpr std::array<Info, kGlobalActionCount> kGlobalInfo{{
#define DEVILDJ_INFO(name, text, repeats) {#name, text, repeats},
    DEVILDJ_GLOBAL_ACTIONS(DEVILDJ_INFO)
#undef DEVILDJ_INFO
}};

const Info& info(CommandId id)
{
    if (isDeckCommand(id))
        return kDeckInfo[static_cast<std::size_t>(commandDeckAction(id))];
    return kGlobalInfo[static_cast<std::size_t>(static_cast<int>(id) - kCommandDecks * kDeckActionCount)];
}
} // namespace

std::string commandName(CommandId id)
{
    if (static_cast<int>(id) >= kCommandCount)
        return "?";
    if (isDeckCommand(id))
        return std::string("Deck ") + static_cast<char>('A' + commandDeck(id)) + ": " + info(id).text;
    return info(id).text;
}

std::string commandKey(CommandId id)
{
    if (static_cast<int>(id) >= kCommandCount)
        return "?";
    if (isDeckCommand(id))
        return std::string("deck") + static_cast<char>('A' + commandDeck(id)) + "." + info(id).key;
    return std::string("global.") + info(id).key;
}

bool commandRepeats(CommandId id)
{
    return static_cast<int>(id) < kCommandCount && info(id).repeats;
}

std::string commandShortName(CommandId id)
{
    if (static_cast<int>(id) >= kCommandCount)
        return "?";
    if (isDeckCommand(id))
    {
        using DA = DeckAction;
        const int a = static_cast<int>(commandDeckAction(id));
        if (a >= static_cast<int>(DA::Pad1) && a <= static_cast<int>(DA::Pad8))
            return "Pad " + std::to_string(a - static_cast<int>(DA::Pad1) + 1);
        if (a >= static_cast<int>(DA::PadAlt1) && a <= static_cast<int>(DA::PadAlt8))
            return "Pad " + std::to_string(a - static_cast<int>(DA::PadAlt1) + 1) + "'";
        switch (commandDeckAction(id))
        {
            case DA::Cue:
                return "Cue";
            case DA::Play:
                return "Play";
            case DA::Sync:
                return "Sync";
            case DA::AutoLoop:
                return "Loop 4";
            case DA::BendDown:
                return "Bend -";
            case DA::BendUp:
                return "Bend +";
            case DA::LoopHalve:
                return "Loop /2";
            case DA::LoopDouble:
                return "Loop x2";
            case DA::BeatJumpBack:
                return "Jump <";
            case DA::BeatJumpForward:
                return "Jump >";
            case DA::TempoDown:
                return "Tempo -";
            case DA::TempoUp:
                return "Tempo +";
            case DA::FilterToLowPass:
                return "Filt LP";
            case DA::FilterToHighPass:
                return "Filt HP";
            case DA::FilterReset:
                return "Filt 0";
            case DA::JumpToStart:
                return "Start";
            case DA::Reverse:
                return "Rev";
            case DA::KeySync:
                return "KeySync";
            case DA::Reloop:
                return "Reloop";
            case DA::KillLow:
                return "Kill LO";
            case DA::KillMid:
                return "Kill MID";
            case DA::KillHigh:
                return "Kill HI";
            case DA::Slip:
                return "Slip";
            case DA::KeyLock:
                return "KeyLock";
            case DA::HeadphoneCue:
                return "Phones";
            case DA::MakeMaster:
                return "Master";
            case DA::Load:
                return "Load";
            default:
                return info(id).key;
        }
    }
    using GA = GlobalAction;
    switch (static_cast<GA>(static_cast<int>(id) - kCommandDecks * kDeckActionCount))
    {
        case GA::CrossfaderLeft:
            return "XF <";
        case GA::CrossfaderRight:
            return "XF >";
        case GA::CrossfaderSnapLeft:
            return "XF |<";
        case GA::CrossfaderSnapRight:
            return "XF >|";
        case GA::CrossfaderCentre:
            return "XF mid";
        case GA::AutoTransition:
            return "Auto mix";
        case GA::AutoTransitionCancel:
            return "Auto off";
        case GA::LoadSuggestion:
            return "Load next";
        case GA::NextSuggestion:
            return "Next sug.";
        case GA::PreviewSuggestion:
            return "Preview";
        case GA::CycleIntent:
            return "Intent";
        case GA::RateTransitionUp:
            return "Good";
        case GA::RateTransitionDown:
            return "Bad";
        case GA::Quantize:
            return "Quantize";
        case GA::PadModeHotCue:
            return "HotCue";
        case GA::PadModeBeatLoop:
            return "BeatLoop";
        case GA::PadModeBeatJump:
            return "BeatJump";
        case GA::PadModeSlicer:
            return "Slicer";
        case GA::PadModeKeyShift:
            return "KeyShift";
        case GA::PadModeKeyboard:
            return "Keys";
        case GA::PadModePadFx:
            return "Pad FX";
        case GA::PadModeSampler:
            return "Sampler";
        case GA::Fx1Toggle:
            return "FX1";
        case GA::Fx1Next:
            return "FX1 >";
        case GA::Fx2Toggle:
            return "FX2";
        case GA::Fx2Next:
            return "FX2 >";
        case GA::Fx1Target:
            return "FX1 to";
        case GA::Fx2Target:
            return "FX2 to";
        case GA::Record:
            return "Rec";
        case GA::Automix:
            return "Automix";
        case GA::FullScreen:
            return "Full";
        case GA::DebugOverlay:
            return "Debug";
        case GA::Panic:
            return "Panic";
        case GA::KeymapOverlay:
            return "Keymap";
        case GA::BrowserSearch:
            return "Search";
        case GA::BrowserBigger:
            return "Big";
        case GA::BrowserRemove:
            return "Remove";
        case GA::PreviewTrack:
            return "Preview";
        case GA::SwitchPanel:
            return "Panel";
        default:
        {
            // "Browser: up" -> "up"; "Beat Maker: track 1" -> "track 1".
            std::string text = info(id).text;
            if (const auto colon = text.find(": "); colon != std::string::npos)
                text = text.substr(colon + 2);
            if (const auto paren = text.find(" ("); paren != std::string::npos)
                text = text.substr(0, paren);
            return text;
        }
    }
}

std::optional<CommandId> commandFromKey(const std::string& key)
{
    for (int i = 0; i < kCommandCount; ++i)
        if (commandKey(static_cast<CommandId>(i)) == key)
            return static_cast<CommandId>(i);
    return std::nullopt;
}

std::vector<CommandId> allCommands()
{
    std::vector<CommandId> ids;
    ids.reserve(kCommandCount);
    for (int i = 0; i < kCommandCount; ++i)
        ids.push_back(static_cast<CommandId>(i));
    return ids;
}

} // namespace devildj::ui
