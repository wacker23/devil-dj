#include "MixerControls.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace devildj::ui
{

namespace
{
const juce::Colour kTrack(0xff2a2a33);
const juce::Colour kText(0xffd8d8e0);
const juce::Colour kCap(0xffe8e8ee);
} // namespace

ValueControl::ValueControl(juce::String labelText, Style s, double defaultVal)
    : label(std::move(labelText))
    , style(s)
    , defaultValue(defaultVal)
    , value(defaultVal)
{
    setRepaintsOnMouseActivity(true);
    setMouseCursor(style == Style::HorizontalFader ? juce::MouseCursor::LeftRightResizeCursor
                                                   : juce::MouseCursor::UpDownResizeCursor);
}

void ValueControl::setValue(double newValue, bool notify)
{
    newValue = std::clamp(newValue, 0.0, 1.0);
    if (newValue == value)
        return;
    value = newValue;
    if (notify && onChange)
        onChange(value);
    repaint();
}

void ValueControl::change(double newValue)
{
    setValue(newValue, true);
    if (valueText)
        setTooltip(label + ": " + valueText(value));
}

float ValueControl::pixelsPerRange() const
{
    switch (style)
    {
        case Style::VerticalFader:
            return std::max(20.0f, static_cast<float>(getHeight()) - 24.0f); // the cap follows the finger
        case Style::HorizontalFader:
            return std::max(20.0f, static_cast<float>(getWidth()) - 30.0f);
        case Style::Knob:
        default:
            return kKnobPixelsPerRange;
    }
}

void ValueControl::dragBy(float pixels, bool fine)
{
    change(value + pixels / pixelsPerRange() * (fine ? kFineFactor : 1.0));
}

void ValueControl::wheel(double notches, bool fine)
{
    change(value + notches * kWheelStep * (fine ? kFineFactor : 1.0));
}

void ValueControl::resetToDefault()
{
    change(defaultValue);
}

void ValueControl::mouseDown(const juce::MouseEvent& e)
{
    dragging = true;
    lastPos = e.position;
    repaint();
}

void ValueControl::mouseDrag(const juce::MouseEvent& e)
{
    const auto delta = e.position - lastPos;
    lastPos = e.position;
    const float pixels = style == Style::HorizontalFader ? delta.x : -delta.y;
    if (pixels != 0.0f)
        dragBy(pixels, e.mods.isAltDown());
}

void ValueControl::mouseUp(const juce::MouseEvent&)
{
    dragging = false;
    repaint();
}

void ValueControl::mouseDoubleClick(const juce::MouseEvent&)
{
    resetToDefault();
}

void ValueControl::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const float amount = std::abs(w.deltaY) >= std::abs(w.deltaX) ? w.deltaY : w.deltaX;
    wheel((w.isReversed ? -amount : amount) / 0.25, e.mods.isAltDown()); // ~0.25 per notch
}

void ValueControl::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    const bool hot = dragging || isMouseOver();
    g.setFont(juce::FontOptions(10.5f, juce::Font::bold));

    if (style == Style::Knob)
    {
        // Wide bounds: knob on the left, label beside it; otherwise the label goes underneath.
        const bool sideLabel = area.getWidth() >= area.getHeight() * 1.6f;
        auto labelArea = sideLabel ? area.removeFromRight(area.getWidth() - area.getHeight()).withTrimmedLeft(4.0f)
                                   : area.removeFromBottom(13.0f);
        const float size = std::min(area.getWidth(), area.getHeight()) - 2.0f;
        const auto knob = area.withSizeKeepingCentre(size, size);
        const float r = size * 0.5f;
        const auto c = knob.getCentre();
        constexpr float start = -0.75f * std::numbers::pi_v<float>, end = 0.75f * std::numbers::pi_v<float>;
        const float angle = start + static_cast<float>(value) * (end - start);
        const float detent = start + static_cast<float>(defaultValue) * (end - start);

        juce::Path track;
        track.addCentredArc(c.x, c.y, r - 2.0f, r - 2.0f, 0.0f, start, end, true);
        g.setColour(kTrack);
        g.strokePath(track, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        juce::Path arc;
        arc.addCentredArc(c.x, c.y, r - 2.0f, r - 2.0f, 0.0f, std::min(detent, angle), std::max(detent, angle), true);
        g.setColour(accent);
        g.strokePath(arc, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setColour(hot ? juce::Colour(0xff3a3a46) : juce::Colour(0xff30303a));
        g.fillEllipse(knob.reduced(6.0f));
        g.setColour(kCap);
        const juce::Point<float> tip(c.x + (r - 8.0f) * std::sin(angle), c.y - (r - 8.0f) * std::cos(angle));
        g.drawLine(c.x + 3.0f * std::sin(angle), c.y - 3.0f * std::cos(angle), tip.x, tip.y, 2.0f);

        g.setColour(kText);
        g.drawText(dragging && valueText ? valueText(value) : label, labelArea,
                   sideLabel ? juce::Justification::centredLeft : juce::Justification::centred);
        return;
    }

    const bool vertical = style == Style::VerticalFader;
    auto labelArea = vertical ? area.removeFromBottom(13.0f) : area.removeFromLeft(0.0f);
    const auto slot = vertical ? area.withSizeKeepingCentre(4.0f, area.getHeight() - 22.0f)
                               : area.withSizeKeepingCentre(area.getWidth() - 28.0f, 4.0f);
    g.setColour(kTrack);
    g.fillRoundedRectangle(slot, 2.0f);
    // Centre mark for the crossfader.
    if (!vertical)
    {
        g.setColour(kText.withAlpha(0.4f));
        g.fillRect(slot.getCentreX() - 0.5f, area.getY() + 2.0f, 1.0f, area.getHeight() - 4.0f);
    }
    const float t = static_cast<float>(value);
    const auto capCentre = vertical ? juce::Point<float>(slot.getCentreX(), slot.getBottom() - t * slot.getHeight())
                                    : juce::Point<float>(slot.getX() + t * slot.getWidth(), slot.getCentreY());
    const auto cap = vertical ? juce::Rectangle<float>(28.0f, 16.0f).withCentre(capCentre)
                              : juce::Rectangle<float>(18.0f, area.getHeight() - 6.0f).withCentre(capCentre);
    g.setColour(hot ? kCap : kCap.darker(0.15f));
    g.fillRoundedRectangle(cap, 3.0f);
    g.setColour(accent);
    if (vertical)
        g.fillRect(cap.getX() + 3.0f, cap.getCentreY() - 1.0f, cap.getWidth() - 6.0f, 2.0f);
    else
        g.fillRect(cap.getCentreX() - 1.0f, cap.getY() + 3.0f, 2.0f, cap.getHeight() - 6.0f);
    if (vertical)
    {
        g.setColour(kText);
        g.drawText(dragging && valueText ? valueText(value) : label, labelArea, juce::Justification::centred);
    }
}

// ---- meter ----

LevelMeterView::LevelMeterView(Source s)
    : source(std::move(s))
{
    setInterceptsMouseClicks(false, false);
}

float LevelMeterView::levelToProportion(float linear) noexcept
{
    if (linear <= 0.0f)
        return 0.0f;
    const float db = 20.0f * std::log10(linear);
    return std::clamp((db + 48.0f) / 54.0f, 0.0f, 1.0f);
}

void LevelMeterView::paint(juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff0d0d10));
    g.fillRoundedRectangle(area, 2.0f);
    const auto [l, r] = source ? source() : std::pair<float, float>{0.0f, 0.0f};
    const float barWidth = (area.getWidth() - 3.0f) / 2.0f;
    const float zeroDb = area.getBottom() - levelToProportion(1.0f) * area.getHeight();
    int index = 0;
    for (float level : {l, r})
    {
        const float x = area.getX() + 1.0f + static_cast<float>(index++) * (barWidth + 1.0f);
        const float top = area.getBottom() - levelToProportion(level) * area.getHeight();
        juce::ColourGradient grad(juce::Colour(0xff3cc46a), x, area.getBottom(), juce::Colour(0xffe0483c), x,
                                  area.getY(), false);
        grad.addColour(levelToProportion(std::pow(10.0f, -9.0f / 20.0f)), juce::Colour(0xfff0c030)); // -9 dB
        g.setGradientFill(grad);
        g.fillRect(x, top, barWidth, area.getBottom() - top);
    }
    g.setColour(juce::Colours::white.withAlpha(0.35f));
    g.fillRect(area.getX(), zeroDb, area.getWidth(), 1.0f); // 0 dB line
}

} // namespace devildj::ui
