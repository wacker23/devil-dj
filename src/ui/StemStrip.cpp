#include "ui/StemStrip.h"

#include <cmath>

namespace devildj::ui
{

namespace
{
const juce::Colour kPanel(0xff24272e);
const juce::Colour kDim(0xff6b7080);
constexpr float kGap = 4.0f;
constexpr float kWaveWidth = 44.0f;
constexpr float kDragPixelsForFull = 80.0f; // a full 0..100 % swing
} // namespace

const juce::Colour& StemStrip::stemColour(int stem) noexcept
{
    static const std::array<juce::Colour, kStems> colours{juce::Colour(0xfff0a030), juce::Colour(0xff3c8ce0),
                                                          juce::Colour(0xff3cc46a), juce::Colour(0xffd05ad0)};
    return colours[static_cast<std::size_t>(std::clamp(stem, 0, kStems - 1))];
}

const char* StemStrip::stemLabel(int stem) noexcept
{
    static const char* const labels[kStems] = {"DRUMS", "BASS", "OTHER", "VOCALS"};
    return labels[std::clamp(stem, 0, kStems - 1)];
}

void StemStrip::setState(const engine::DeckState& s)
{
    const bool was = loaded;
    bool changed = s.stemsLoaded != loaded || s.stemMute != mute || s.stemSolo != solo || s.stemFx != fx;
    for (int i = 0; i < kStems; ++i)
        changed = changed || s.stemLevel[static_cast<std::size_t>(i)] != level[static_cast<std::size_t>(i)];
    loaded = s.stemsLoaded;
    mute = s.stemMute;
    solo = s.stemSolo;
    fx = s.stemFx;
    level = s.stemLevel;
    if (loaded != was || getTooltip().isEmpty())
        setTooltip(loaded ? "Stems: click = mute, Shift+click = solo, drag / wheel = level, Ctrl+click = 100 %, "
                            "right-click = FX send on / off. WAVE: the stems in the waveform."
                          : "No stems for this track: right-click it in the library > Separate stems.");
    if (changed)
        repaint();
}

void StemStrip::setWaveView(bool on)
{
    if (wave == on)
        return;
    wave = on;
    repaint();
    if (onWaveView)
        onWaveView(on);
}

juce::Rectangle<float> StemStrip::cellBounds(int cell) const noexcept
{
    auto area = getLocalBounds().toFloat();
    const auto waveArea = area.removeFromRight(kWaveWidth);
    if (cell == kStems)
        return waveArea;
    area.removeFromRight(kGap);
    const float w = (area.getWidth() - kGap * (kStems - 1)) / kStems;
    return {area.getX() + static_cast<float>(cell) * (w + kGap), area.getY(), w, area.getHeight()};
}

int StemStrip::cellAt(float x) const noexcept
{
    for (int c = 0; c <= kStems; ++c)
    {
        const auto b = cellBounds(c);
        if (x >= b.getX() && x < b.getRight())
            return c;
    }
    return -1;
}

void StemStrip::paint(juce::Graphics& g)
{
    const bool anySolo = solo != 0;
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    for (int s = 0; s < kStems; ++s)
    {
        const auto b = cellBounds(s);
        const auto bit = static_cast<std::uint8_t>(1u << s);
        const bool heard = loaded && (anySolo ? (solo & bit) != 0 : (mute & bit) == 0);
        const auto colour = stemColour(s);
        g.setColour(kPanel);
        g.fillRoundedRectangle(b, 4.0f);
        if (loaded) // the level, filled from the left
        {
            const auto fill = b.withWidth(b.getWidth() * std::clamp(level[static_cast<std::size_t>(s)], 0.0f, 1.0f));
            g.setColour(colour.withAlpha(heard ? 0.85f : 0.18f));
            g.fillRoundedRectangle(fill, 4.0f);
        }
        if (loaded && (solo & bit))
        {
            g.setColour(juce::Colours::white);
            g.drawRoundedRectangle(b.reduced(1.0f), 4.0f, 2.0f);
        }
        g.setColour(!loaded ? kDim : heard ? juce::Colours::black : colour.withAlpha(0.7f));
        auto text = b.reduced(4.0f, 0.0f);
        if (loaded && !(fx & bit))
        {
            g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
            g.drawText("DRY", text.removeFromRight(24.0f), juce::Justification::centredRight);
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        }
        g.drawText(stemLabel(s), text, juce::Justification::centredLeft);
    }
    const auto w = cellBounds(kStems);
    g.setColour(wave && loaded ? juce::Colour(0xffe0e0e0) : kPanel);
    g.fillRoundedRectangle(w, 4.0f);
    g.setColour(!loaded ? kDim : wave ? juce::Colours::black : juce::Colours::white);
    g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    g.drawText("WAVE", w, juce::Justification::centred);
}

void StemStrip::send(int stem, engine::StemParam param, float value)
{
    if (onStem)
        onStem(stem, param, value);
}

void StemStrip::mouseDown(const juce::MouseEvent& e)
{
    pressed = cellAt(e.position.x);
    dragged = false;
    dragStart = e.position;
    if (!loaded || pressed < 0)
        return;
    if (pressed == kStems)
    {
        setWaveView(!wave);
        pressed = -1;
        return;
    }
    const auto bit = static_cast<std::uint8_t>(1u << pressed);
    if (e.mods.isPopupMenu())
    {
        send(pressed, engine::StemParam::Fx, (fx & bit) ? 0.0f : 1.0f);
        pressed = -1;
        return;
    }
    dragStartLevel = level[static_cast<std::size_t>(pressed)];
}

void StemStrip::mouseDrag(const juce::MouseEvent& e)
{
    if (!loaded || pressed < 0 || pressed >= kStems)
        return;
    const auto d = e.position - dragStart;
    const float moved = std::abs(d.x) > std::abs(d.y) ? d.x : -d.y; // right or up = louder
    if (!dragged && std::abs(moved) < 4.0f)
        return;
    dragged = true;
    send(pressed, engine::StemParam::Level, std::clamp(dragStartLevel + moved / kDragPixelsForFull, 0.0f, 1.0f));
}

void StemStrip::mouseUp(const juce::MouseEvent& e)
{
    if (loaded && pressed >= 0 && pressed < kStems && !dragged && cellAt(e.position.x) == pressed)
    {
        const auto bit = static_cast<std::uint8_t>(1u << pressed);
        if (e.mods.isCtrlDown())
            send(pressed, engine::StemParam::Level, 1.0f);
        else if (e.mods.isShiftDown())
            send(pressed, engine::StemParam::Solo, (solo & bit) ? 0.0f : 1.0f);
        else
            send(pressed, engine::StemParam::Mute, (mute & bit) ? 0.0f : 1.0f);
    }
    pressed = -1;
}

void StemStrip::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const int c = cellAt(e.position.x);
    if (!loaded || c < 0 || c >= kStems)
        return;
    const float step = wheel.deltaY > 0.0f ? 0.05f : wheel.deltaY < 0.0f ? -0.05f : 0.0f;
    send(c, engine::StemParam::Level, std::clamp(level[static_cast<std::size_t>(c)] + step, 0.0f, 1.0f));
}

} // namespace devildj::ui
