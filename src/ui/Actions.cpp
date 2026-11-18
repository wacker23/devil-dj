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

std::vector<CommandId> allCommands()
{
    std::vector<CommandId> ids;
    ids.reserve(kCommandCount);
    for (int i = 0; i < kCommandCount; ++i)
        ids.push_back(static_cast<CommandId>(i));
    return ids;
}

} // namespace devildj::ui
