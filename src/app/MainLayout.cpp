#include "MainLayout.h"

#include <algorithm>
#include <cmath>

namespace devildj::app
{

MainLayout computeLayout(juce::Rectangle<int> window, WaveLayout layout, bool browserBig, const LayoutSizes& s)
{
    MainLayout out;
    const bool vertical = layout == WaveLayout::Vertical;
    out.wavesVertical = vertical;
    out.wavesVisible = !browserBig;
    out.panelVisible = !browserBig;
    auto area = window.reduced(12).withTrimmedBottom(18); // bottom: the build-info line
    // The top row: the mixer gives up height first on short windows (down to its minimum), so what is below keeps
    // room - two ~80 px waveform rows (horizontal only), the panel and ~4 browser rows.
    const int below = (vertical ? 0 : 2 * 80 + 6) + 130 + 14 + s.panelHeight + 6;
    const int topHeight = std::clamp(area.getHeight() - below, s.mixerMinimumHeight, s.mixerPreferredHeight);
    auto top = area.removeFromTop(topHeight);
    area.removeFromTop(8);
    const int waveColumns = vertical && !browserBig ? 2 * (s.verticalWaveWidth + 6) : 0;
    const int deckWidth = std::max(200, (top.getWidth() - s.mixerPreferredWidth - 12 - waveColumns) / 2);
    const int deckHeight = std::min(s.deckMaxHeight, top.getHeight());
    out.deckA = top.removeFromLeft(deckWidth).withHeight(deckHeight);
    top.removeFromLeft(6);
    out.deckB = top.removeFromRight(deckWidth).withHeight(deckHeight);
    top.removeFromRight(6);
    if (waveColumns > 0)
    {
        out.waveA = top.removeFromLeft(s.verticalWaveWidth);
        top.removeFromLeft(6);
        out.waveB = top.removeFromRight(s.verticalWaveWidth);
        top.removeFromRight(6);
    }
    out.mixer = top;
    if (browserBig)
    {
        out.browser = area;
        return out;
    }
    const int browserHeight =
        std::max(160, area.getHeight() - (vertical ? 0 : 2 * 100 + 12) - s.panelHeight - 6);
    out.browser = area.removeFromBottom(browserHeight);
    area.removeFromBottom(6);
    auto panel = area.removeFromBottom(s.panelHeight);
    auto tabs = panel.removeFromLeft(64);
    const int th = tabs.getHeight() / 4;
    for (std::size_t i = 0; i < 3; ++i)
        out.tabs[i] = tabs.removeFromTop(th).reduced(0, 1);
    out.tabs[3] = tabs.reduced(0, 1);
    panel.removeFromLeft(4);
    out.panel = panel;
    if (!vertical)
    {
        area.removeFromBottom(6);
        const int waveHeight = (area.getHeight() - 6) / 2;
        out.waveA = area.removeFromTop(waveHeight);
        area.removeFromTop(6);
        out.waveB = area.removeFromTop(waveHeight);
    }
    return out;
}

juce::AffineTransform verticalWaveTransform(juce::Rectangle<int> column)
{
    // A quarter turn clockwise maps the logical (x, y) to (-y, x); then into the column: x = 0 at its top, the logical
    // top edge (y = 0) at its right.
    return juce::AffineTransform::rotation(juce::MathConstants<float>::halfPi)
        .translated(static_cast<float>(column.getRight()), static_cast<float>(column.getY()));
}

} // namespace devildj::app
