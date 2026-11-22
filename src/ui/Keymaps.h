#pragma once

#include "ui/KeyboardController.h"

#include <string>
#include <vector>

namespace devildj::ui
{

// Whole keymaps (P3-20): presets, saving / loading a full map, and the edits the keymap editor makes.
enum class KeymapPreset
{
    Default,       // docs/KEYMAP.md
    LaptopCompact, // the same, with the F-key actions on Alt + the number row (no Fn needed)
};
std::vector<KeyBinding> presetKeymap(KeymapPreset preset);
const char* presetName(KeymapPreset preset);

// A full map as JSON: { "version": 1, "replace": true, "bindings": [ {"action", "key", "context"}, ... ] }.
// keymapFromJson takes that (the map exactly) or an override file without "replace" (on top of the default map,
// as keymap.json has been since P1-16). Problems go to `errors` and that entry is skipped.
std::string keymapToJson(const std::vector<KeyBinding>& bindings);
std::vector<KeyBinding> keymapFromJson(const std::string& json, std::vector<std::string>& errors);

// The editor's edits. assignKey: the action gets `chord` in `context` (its other keys there go), and whatever had the
// chord loses it (returned in `displaced`). clearKey: the action has no key in `context`.
struct Assignment
{
    std::vector<KeyBinding> bindings;
    std::vector<CommandId> displaced;
};
Assignment assignKey(std::vector<KeyBinding> bindings, CommandId command, const KeyChord& chord,
                     KeyContext context = KeyContext::Main);
std::vector<KeyBinding> clearKey(std::vector<KeyBinding> bindings, CommandId command,
                                 KeyContext context = KeyContext::Main);
// The keys of an action ("Z, Shift+X"; "" for none).
std::string keysOf(const std::vector<KeyBinding>& bindings, CommandId command, KeyContext context = KeyContext::Main);
// Who else has this chord (nothing if free).
std::optional<CommandId> holderOf(const std::vector<KeyBinding>& bindings, const KeyChord& chord,
                                  KeyContext context = KeyContext::Main);

} // namespace devildj::ui
