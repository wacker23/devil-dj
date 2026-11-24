#pragma once

#include "engine/EngineCommand.h"
#include "engine/EngineState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace devildj::ui
{

// A deck's four stem buttons (P6-04): DRUMS / BASS / OTHER / VOCALS, plus WAVE (the stem waveform view).
//   click                mute / unmute
//   Shift + click        solo / unsolo
//   drag up / down       level (also left / right); Ctrl + click = back to 100 %
//   wheel                level
//   right-click          FX send on / off (off: the stem skips the deck's effects, "DRY")
// Each button fills to its level, dims when muted (or not soloed while another is), and is outlined while
// soloed. Without stems for the track every button is dimmed and the tooltip says how to make them.
class StemStrip final : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr int kStems = 4;
    static const juce::Colour& stemColour(int stem) noexcept;
    static const char* stemLabel(int stem) noexcept;

    std::function<void(int stem, engine::StemParam param, float value)> onStem;
    std::function<void(bool on)> onWaveView;

    void setState(const engine::DeckState& state); // from the deck's refresh, every frame
    bool waveView() const noexcept { return wave; }
    void setWaveView(bool on);

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    // Which button (0..3 a stem, 4 = WAVE, -1 = none) is under x (public for tests).
    int cellAt(float x) const noexcept;
    juce::Rectangle<float> cellBounds(int cell) const noexcept;

private:
    void send(int stem, engine::StemParam param, float value);
    bool loaded = false;
    std::array<float, kStems> level{1.0f, 1.0f, 1.0f, 1.0f};
    std::uint8_t mute = 0, solo = 0, fx = 0x0f;
    bool wave = false;
    int pressed = -1;
    bool dragged = false;
    float dragStartLevel = 1.0f;
    juce::Point<float> dragStart;
};

} // namespace devildj::ui
