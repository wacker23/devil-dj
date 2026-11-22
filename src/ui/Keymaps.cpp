#include "ui/Keymaps.h"

#include "ui/Actions.h"

#include <juce_core/juce_core.h>

#include <algorithm>

namespace devildj::ui
{

std::vector<KeyBinding> presetKeymap(KeymapPreset preset)
{
    auto map = defaultKeymap();
    if (preset == KeymapPreset::LaptopCompact)
    {
        // F1 .. F10 -> Alt+1 .. Alt+0, Shift+F1 .. Shift+F10 -> Alt+Shift+1 .. 0 (F11 / F12 stay: full screen and the
        // overlay are rarely needed mid-set).
        const std::uint16_t fkeys[] = {scan::F1, scan::F2, scan::F3, scan::F4, scan::F5,
                                       scan::F6, scan::F7, scan::F8, scan::F9, scan::F10};
        const std::uint16_t digits[] = {scan::Digit1, scan::Digit2, scan::Digit3, scan::Digit4, scan::Digit5,
                                        scan::Digit6, scan::Digit7, scan::Digit8, scan::Digit9, scan::Digit0};
        for (auto& b : map)
            for (int i = 0; i < 10; ++i)
                if (b.chord.scanCode == fkeys[i])
                {
                    b.chord.scanCode = digits[i];
                    b.chord.modifiers = static_cast<std::uint8_t>(b.chord.modifiers | mods::Alt);
                }
    }
    return map;
}

const char* presetName(KeymapPreset preset)
{
    return preset == KeymapPreset::LaptopCompact ? "Laptop compact" : "Default";
}

std::string keymapToJson(const std::vector<KeyBinding>& bindings)
{
    auto sorted = bindings;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const KeyBinding& a, const KeyBinding& b) { return commandKey(a.command) < commandKey(b.command); });
    juce::Array<juce::var> list;
    for (const auto& b : sorted)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("action", juce::String(commandKey(b.command)));
        o->setProperty("key", juce::String(describeChord(b.chord)));
        if (b.context != KeyContext::Main)
            o->setProperty("context", juce::String(contextName(b.context)));
        list.add(juce::var(o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty("version", 1);
    root->setProperty("replace", true);
    root->setProperty("bindings", list);
    return juce::JSON::toString(juce::var(root), false).toStdString();
}

std::vector<KeyBinding> keymapFromJson(const std::string& json, std::vector<std::string>& errors)
{
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8(json.c_str()));
    const bool replace = static_cast<bool>(parsed.getProperty("replace", false));
    // A full map: its bindings exactly (applied to an empty map, they can't displace each other unless they clash).
    return applyKeymapOverrides(replace ? std::vector<KeyBinding>{} : defaultKeymap(), json, errors);
}

Assignment assignKey(std::vector<KeyBinding> bindings, CommandId command, const KeyChord& chord, KeyContext context)
{
    Assignment out;
    for (const auto& b : bindings)
        if (b.context == context && b.chord == chord && b.command != command)
            out.displaced.push_back(b.command);
    std::erase_if(bindings, [&](const KeyBinding& b)
                  { return b.context == context && (b.command == command || b.chord == chord); });
    bindings.push_back({chord, command, context});
    out.bindings = std::move(bindings);
    return out;
}

std::vector<KeyBinding> clearKey(std::vector<KeyBinding> bindings, CommandId command, KeyContext context)
{
    std::erase_if(bindings, [&](const KeyBinding& b) { return b.context == context && b.command == command; });
    return bindings;
}

std::string keysOf(const std::vector<KeyBinding>& bindings, CommandId command, KeyContext context)
{
    std::string text;
    for (const auto& b : bindings)
        if (b.command == command && b.context == context)
            text += (text.empty() ? "" : ", ") + describeChord(b.chord);
    return text;
}

std::optional<CommandId> holderOf(const std::vector<KeyBinding>& bindings, const KeyChord& chord, KeyContext context)
{
    for (const auto& b : bindings)
        if (b.context == context && b.chord == chord)
            return b.command;
    return std::nullopt;
}

} // namespace devildj::ui
