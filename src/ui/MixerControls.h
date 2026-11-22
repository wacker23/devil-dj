#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace devildj::ui
{

// A knob or fader with a normalised value 0..1 (P1-15). Same gestures for every style:
//   drag        knobs: vertical (up = more); faders: along the fader, following the finger / pointer
//   Alt + drag  fine (1/10 the speed)
//   wheel       2 % per notch (Alt: 0.2 %)
//   double-click / double-tap   back to the default
// Touch arrives as mouse events, and each finger is its own input source, so two faders can move at once.
class ValueControl final : public juce::Component, public juce::SettableTooltipClient
{
public:
    enum class Style
    {
        Knob,
        VerticalFader,
        HorizontalFader,
    };

    ValueControl(juce::String label, Style style, double defaultValue);

    std::function<void(double value)> onChange;          // user changes only
    std::function<juce::String(double value)> valueText; // shown while dragging and in the tooltip

    void setValue(double value, bool notify);
    double getValue() const noexcept { return value; }
    double getDefault() const noexcept { return defaultValue; }
    bool isBeingChanged() const noexcept { return dragging; }
    void setAccent(juce::Colour c) { accent = c; }
    void setLabel(const juce::String& text)
    {
        if (text != label)
        {
            label = text;
            repaint();
        }
    }
    const juce::String& getLabel() const noexcept { return label; }

    static constexpr float kKnobPixelsPerRange = 180.0f;
    static constexpr double kFineFactor = 0.1;
    static constexpr double kWheelStep = 0.02;

    // The gesture paths the mouse handlers use (public for tests).
    void dragBy(float pixels, bool fine); // + = up (vertical) / right (horizontal)
    void wheel(double notches, bool fine);
    void resetToDefault();

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

private:
    float pixelsPerRange() const;
    void change(double newValue);

    juce::String label;
    Style style;
    double defaultValue;
    double value;
    bool dragging = false;
    juce::Point<float> lastPos;
    juce::Colour accent{0xffe0483c};
};

// Stereo peak meter, -48 ... +6 dB, reading linear peaks from a callback at display rate.
class LevelMeterView final : public juce::Component
{
public:
    using Source = std::function<std::pair<float, float>()>; // peak L, peak R (linear)
    explicit LevelMeterView(Source source);
    void paint(juce::Graphics& g) override;
    static float levelToProportion(float linear) noexcept; // 0 at -48 dB, 1 at +6 dB

private:
    Source source;
};

} // namespace devildj::ui
