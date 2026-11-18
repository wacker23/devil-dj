#include "KeymapOverlay.h"

#include <algorithm>

namespace devildj::ui
{

namespace
{
const juce::Colour kKey(0xff24242c);
const juce::Colour kDeckA(0xff5a2020);
const juce::Colour kDeckB(0xff1f3550);
const juce::Colour kLegend(0xff9a9aa6);
const juce::Colour kPlain(0xfff0f0f5);
const juce::Colour kShift(0xfff0a030);
const juce::Colour kCtrl(0xff6ec0f0);
} // namespace

const std::vector<KeymapOverlay::KeyCap>& KeymapOverlay::layout()
{
    using namespace scan;
    static const std::vector<KeyCap> keys = []
    {
        std::vector<KeyCap> k;
        // Function row.
        k.push_back({Escape, "Esc", 0.0f, 0.0f, 1.0f});
        const std::uint16_t f[] = {F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12};
        const char* const fn[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
        for (int i = 0; i < 12; ++i)
            k.push_back({f[i], fn[i], 2.0f + static_cast<float>(i) + 0.5f * static_cast<float>(i / 4), 0.0f, 1.0f});
        // Number row.
        const std::uint16_t numbers[] = {Backquote, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6,
                                         Digit7,    Digit8, Digit9, Digit0, Minus,  Equals};
        const char* const numberNames[] = {"`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "="};
        for (int i = 0; i < 13; ++i)
            k.push_back({numbers[i], numberNames[i], static_cast<float>(i), 1.25f, 1.0f});
        k.push_back({0, "Bksp", 13.0f, 1.25f, 2.0f});
        // Q row.
        k.push_back({Tab, "Tab", 0.0f, 2.25f, 1.5f});
        const std::uint16_t qRow[] = {Q, W, E, R, T, Y, U, I, O, P};
        const char* const qNames[] = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"};
        for (int i = 0; i < 10; ++i)
            k.push_back({qRow[i], qNames[i], 1.5f + static_cast<float>(i), 2.25f, 1.0f});
        k.push_back({0, "[", 11.5f, 2.25f, 1.0f});
        k.push_back({0, "]", 12.5f, 2.25f, 1.0f});
        k.push_back({0, "\\", 13.5f, 2.25f, 1.5f});
        // A row.
        k.push_back({0, "Caps", 0.0f, 3.25f, 1.75f});
        const std::uint16_t aRow[] = {A, S, D, F, G, H, J, K, L, Semicolon, Quote};
        const char* const aNames[] = {"A", "S", "D", "F", "G", "H", "J", "K", "L", ";", "'"};
        for (int i = 0; i < 11; ++i)
            k.push_back({aRow[i], aNames[i], 1.75f + static_cast<float>(i), 3.25f, 1.0f});
        k.push_back({Enter, "Enter", 12.75f, 3.25f, 2.25f});
        // Z row.
        k.push_back({0, "Shift", 0.0f, 4.25f, 2.25f});
        const std::uint16_t zRow[] = {Z, X, C, V, B, N, M, Comma, Period, Slash};
        const char* const zNames[] = {"Z", "X", "C", "V", "B", "N", "M", ",", ".", "/"};
        for (int i = 0; i < 10; ++i)
            k.push_back({zRow[i], zNames[i], 2.25f + static_cast<float>(i), 4.25f, 1.0f});
        k.push_back({0, "Shift", 12.25f, 4.25f, 2.75f});
        // Bottom row.
        k.push_back({0, "Ctrl", 0.0f, 5.25f, 1.25f});
        k.push_back({0, "Win", 1.25f, 5.25f, 1.25f});
        k.push_back({0, "Alt", 2.5f, 5.25f, 1.25f});
        k.push_back({Space, "Space", 3.75f, 5.25f, 6.25f});
        k.push_back({0, "Alt", 10.0f, 5.25f, 1.25f});
        k.push_back({0, "Win", 11.25f, 5.25f, 1.25f});
        k.push_back({0, "Menu", 12.5f, 5.25f, 1.25f});
        k.push_back({0, "Ctrl", 13.75f, 5.25f, 1.25f});
        // Navigation.
        k.push_back({PageUp, "PgUp", 15.5f, 1.25f, 1.0f});
        k.push_back({PageDown, "PgDn", 16.5f, 1.25f, 1.0f});
        k.push_back({Delete, "Del", 15.5f, 2.25f, 1.0f});
        k.push_back({Up, "Up", 16.5f, 4.25f, 1.0f});
        k.push_back({Left, "Left", 15.5f, 5.25f, 1.0f});
        k.push_back({Down, "Down", 16.5f, 5.25f, 1.0f});
        k.push_back({Right, "Right", 17.5f, 5.25f, 1.0f});
        return k;
    }();
    return keys;
}

std::unordered_map<std::uint16_t, KeymapOverlay::Labels>
KeymapOverlay::labelsFor(const std::vector<KeyBinding>& bindings, KeyContext context)
{
    std::unordered_map<std::uint16_t, Labels> out;
    for (const auto& b : bindings)
    {
        if (b.context != context || (b.chord.modifiers & mods::Alt) != 0)
            continue;
        auto& l = out[b.chord.scanCode];
        const auto text = commandShortName(b.command);
        const int deck = commandDeck(b.command);
        switch (b.chord.modifiers)
        {
            case mods::None:
                l.plain = text;
                if (deck >= 0)
                    l.deck = deck;
                break;
            case mods::Shift:
                l.shift = text;
                break;
            case mods::Ctrl:
                l.ctrl = text;
                break;
            default: // Ctrl+Shift: shown on the Ctrl line if that's free
                if (l.ctrl.empty())
                    l.ctrl = "+" + text;
                break;
        }
        if (l.deck < 0 && deck >= 0)
            l.deck = deck;
    }
    return out;
}

KeymapOverlay::KeymapOverlay()
{
    setInterceptsMouseClicks(false, false);
}

void KeymapOverlay::setBindings(const std::vector<KeyBinding>& bindings)
{
    current = labelsFor(bindings);
    repaint();
}

void KeymapOverlay::setStatus(juce::String text)
{
    status = std::move(text);
    repaint();
}

void KeymapOverlay::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(0.78f));
    auto area = getLocalBounds().toFloat().reduced(24.0f);
    auto title = area.removeFromTop(30.0f);
    auto footer = area.removeFromBottom(24.0f);

    g.setColour(kPlain);
    g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    g.drawText("Keyboard map (hold Ctrl + /)", title, juce::Justification::centredLeft);
    g.setColour(kLegend);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(status, title, juce::Justification::centredRight);

    const float u = std::min(area.getWidth() / kLayoutWidth, area.getHeight() / kLayoutHeight);
    const juce::Point<float> origin(area.getCentreX() - u * kLayoutWidth * 0.5f,
                                    area.getCentreY() - u * kLayoutHeight * 0.5f);
    const float gap = std::max(2.0f, u * 0.06f);
    const float small = std::clamp(u * 0.19f, 8.0f, 13.0f);

    for (const auto& key : layout())
    {
        const juce::Rectangle<float> r(origin.x + key.x * u, origin.y + key.y * u, key.width * u - gap, u - gap);
        const auto it = current.find(key.scanCode);
        const Labels* l = key.scanCode != 0 && it != current.end() ? &it->second : nullptr;
        const juce::Colour fill = l == nullptr   ? kKey.withAlpha(0.55f)
                                  : l->deck == 0 ? kDeckA
                                  : l->deck == 1 ? kDeckB
                                                 : kKey;
        g.setColour(fill);
        g.fillRoundedRectangle(r, u * 0.08f);
        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.drawRoundedRectangle(r, u * 0.08f, 1.0f);

        auto inner = r.reduced(u * 0.07f, u * 0.05f);
        g.setColour(kLegend);
        g.setFont(juce::FontOptions(small, juce::Font::bold));
        g.drawText(key.legend, inner.removeFromTop(small + 1.0f), juce::Justification::topLeft);
        if (l == nullptr)
            continue;
        const float line = inner.getHeight() / 3.0f;
        g.setFont(juce::FontOptions(small));
        const std::pair<const std::string*, juce::Colour> rows[] = {
            {&l->plain, kPlain}, {&l->shift, kShift}, {&l->ctrl, kCtrl}};
        for (const auto& [text, colour] : rows)
        {
            auto row = inner.removeFromTop(line);
            if (text->empty())
                continue;
            g.setColour(colour);
            g.drawFittedText(juce::String::fromUTF8(text->c_str()), row.toNearestInt(),
                             juce::Justification::centredLeft, 1, 0.7f);
        }
    }

    g.setFont(juce::FontOptions(13.0f));
    auto legendRow = footer;
    const std::pair<const char*, juce::Colour> legend[] = {{"key alone", kPlain},
                                                           {"with Shift", kShift},
                                                           {"with Ctrl", kCtrl},
                                                           {"deck A", kDeckA.brighter(0.6f)},
                                                           {"deck B", kDeckB.brighter(0.8f)}};
    for (const auto& [text, colour] : legend)
    {
        g.setColour(colour);
        g.drawText(text, legendRow.removeFromLeft(110.0f), juce::Justification::centredLeft);
    }
}

} // namespace devildj::ui
