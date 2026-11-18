#pragma once

#include "analysis/WaveformBuilder.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>

namespace devildj::ui
{

// Devil-DJ's 3-band waveform colours: low = deep red, mid = amber, high = pale ice, drawn as mirrored bars.
inline juce::Colour bandColour(int band) noexcept
{
    switch (band)
    {
        case analysis::Waveform::Low:
            return juce::Colour(0xffc0302a);
        case analysis::Waveform::Mid:
            return juce::Colour(0xfff0a030);
        default:
            return juce::Colour(0xffd8f0ff);
    }
}

// Visual gain per band: highs carry less energy than lows, so they're lifted to stay visible.
inline float bandDisplayGain(int band) noexcept
{
    return band == analysis::Waveform::High ? 1.8f : band == analysis::Waveform::Mid ? 1.3f : 1.0f;
}

// Draws 3-band bars for one column: `peaks` per band (0..1+), centred on `mid` with half-height `half`.
// Tallest first, so a shorter band is never hidden behind a taller one.
inline void drawBandColumn(juce::Graphics& g, float x, float width, float mid, float half, const float* peaks,
                           float alpha)
{
    float h[analysis::Waveform::kBands];
    int order[analysis::Waveform::kBands] = {0, 1, 2};
    for (int b = 0; b < analysis::Waveform::kBands; ++b)
        h[b] = std::min(1.0f, peaks[b] * bandDisplayGain(b)) * half;
    std::sort(order, order + analysis::Waveform::kBands, [&h](int a, int b) { return h[a] > h[b]; });
    for (int b : order)
    {
        if (h[b] < 0.5f)
            continue;
        g.setColour(bandColour(b).withAlpha(alpha));
        g.fillRect(x, mid - h[b], width, 2.0f * h[b]);
    }
}

} // namespace devildj::ui
