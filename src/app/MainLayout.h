#pragma once

#include <juce_graphics/juce_graphics.h>

#include <array>

namespace devildj::app
{

// Where everything goes in the main window (P3-23). Pure: the window's bounds and the layout in, rectangles out
// (MainComponent applies them; tests check them).
//   Horizontal (default) - deck A | mixer | deck B on top; the FX / sampler / ... panel; the two waveforms one above
//                          the other; the browser at the bottom.
//   Vertical             - the waveforms as two tall columns beside the mixer (time running down), so the browser
//                          gets their rows.
//   browserBig (Ctrl+B)  - either layout without the waveforms and the panel: the browser takes their room.
enum class WaveLayout
{
    Horizontal,
    Vertical,
};

struct MainLayout
{
    juce::Rectangle<int> deckA, deckB, mixer;
    juce::Rectangle<int> waveA, waveB; // in the window; vertical ones are columns (draw them rotated)
    bool wavesVisible = true, wavesVertical = false;
    juce::Rectangle<int> panel;                                     // FX / sampler / recorder / automix
    std::array<juce::Rectangle<int>, 4> tabs;                       // its tab buttons
    bool panelVisible = true;
    juce::Rectangle<int> browser;
};

struct LayoutSizes
{
    int mixerPreferredWidth = 372;
    int mixerMinimumHeight = 330, mixerPreferredHeight = 430;
    int deckMaxHeight = 326;
    int panelHeight = 96;
    int verticalWaveWidth = 120;
};

MainLayout computeLayout(juce::Rectangle<int> window, WaveLayout layout, bool browserBig, const LayoutSizes& sizes = {});

// The transform that shows a WaveformView of logical size (length x width) as a column `column` with time running
// down (a quarter turn clockwise). Give the component bounds (0, 0, column.height, column.width) and this transform.
juce::AffineTransform verticalWaveTransform(juce::Rectangle<int> column);

} // namespace devildj::app
