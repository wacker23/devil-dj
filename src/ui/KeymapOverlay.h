#pragma once

#include "ui/KeyboardController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace devildj::ui
{

// Translucent keyboard showing the current bindings (P1-17), shown while Ctrl+/ is held. Each key shows what
// it does on its own, with Shift and with Ctrl; deck A keys are tinted red, deck B keys blue. It's drawn from
// the KeyboardController's live bindings, so a user keymap (keymap.json) shows up as soon as it's loaded.
class KeymapOverlay final : public juce::Component
{
public:
    KeymapOverlay();

    struct KeyCap
    {
        std::uint16_t scanCode; // 0 = not bindable (Backspace, Caps, ...): drawn for orientation only
        const char* legend;
        float x, y, width; // key units (1 = one letter key)
    };
    static const std::vector<KeyCap>& layout(); // US ANSI keyboard + navigation keys
    static constexpr float kLayoutWidth = 18.5f, kLayoutHeight = 6.25f;

    struct Labels
    {
        std::string plain, shift, ctrl;
        int deck = -1; // deck the key mostly belongs to (-1 = global)
    };
    static std::unordered_map<std::uint16_t, Labels> labelsFor(const std::vector<KeyBinding>& bindings,
                                                               KeyContext context = KeyContext::Main);

    void setBindings(const std::vector<KeyBinding>& bindings);
    void setStatus(juce::String text); // e.g. "keymap.json: 2 changes"
    const std::unordered_map<std::uint16_t, Labels>& labels() const noexcept { return current; }

    void paint(juce::Graphics& g) override;

private:
    std::unordered_map<std::uint16_t, Labels> current;
    juce::String status;
};

} // namespace devildj::ui
