#include "WaveformView.h"

#include "ui/WaveformColours.h"

#include <algorithm>
#include <cmath>

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff121216);
const juce::Colour kGrid(0xff26262e);
const juce::Colour kCueColour(0xfff0a030);
constexpr int kStatsWindow = 120; // frames (~2 s at 60 Hz)
} // namespace

WaveformView::WaveformView(engine::AudioEngine& audioEngine, engine::DeckId deckId, TrackLookup trackLookup,
                           LatencySource latencySource)
    : engine(audioEngine)
    , deck(deckId)
    , lookup(std::move(trackLookup))
    , latency(std::move(latencySource))
    , vblank(this, [this] { onVBlank(); })
{
    setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    setOpaque(true);
}

double WaveformView::uiClockSeconds()
{
    return juce::Time::getMillisecondCounterHiRes() / 1000.0;
}

double WaveformView::samplesPerPixel() const noexcept
{
    const double rate = engine.readState().sampleRate > 0.0 ? engine.readState().sampleRate : 48000.0;
    return visibleSeconds() * rate / std::max(1, getWidth());
}

double WaveformView::displayedPosition(const engine::DeckState& s, double sampleRate, double stateTime, double now,
                                       double latencySeconds) noexcept
{
    if (!s.loaded)
        return 0.0;
    const bool moving = s.playing || s.scratching;
    const double sinceState = std::clamp(now - stateTime, 0.0, 0.1); // never run far ahead of a stalled engine
    const double pos =
        moving ? s.positionSamples + s.rate * sampleRate * (sinceState - latencySeconds) : s.positionSamples;
    return std::clamp(pos, 0.0, static_cast<double>(s.lengthSamples));
}

void WaveformView::onVBlank()
{
    const double now = uiClockSeconds();
    if (lastVBlank > 0.0)
    {
        intervalSum += now - lastVBlank;
        if (++intervalCount >= kStatsWindow)
        {
            stats.frameIntervalMs = 1000.0 * intervalSum / intervalCount;
            intervalSum = 0.0;
            intervalCount = 0;
        }
    }
    lastVBlank = now;
    if (latency && ++framesSinceLatency > 60) // the device latency rarely changes; ask once a second
    {
        cachedLatency = latency();
        framesSinceLatency = 0;
    }
    repaint();
}

void WaveformView::paint(juce::Graphics& g)
{
    const double started = juce::Time::getMillisecondCounterHiRes();
    const auto& es = engine.readState();
    const auto& s = es.decks[deck];
    const double rate = es.sampleRate > 0.0 ? es.sampleRate : 48000.0;
    if (s.bufferId != shownBuffer)
    {
        shownBuffer = s.bufferId;
        track = s.loaded && lookup ? lookup(shownBuffer) : nullptr;
    }

    const auto area = getLocalBounds().toFloat();
    g.fillAll(kBackground);
    const float centreX = std::round(area.getCentreX());
    const float mid = area.getCentreY();
    const float half = area.getHeight() * 0.5f - 3.0f;

    if (track != nullptr && !track->waveform.empty())
    {
        const auto& w = track->waveform;
        const double latencySeconds = cachedLatency + es.masterLatencySamples / rate;
        const double pos = displayedPosition(s, rate, es.hostTimeSeconds, uiClockSeconds(), latencySeconds);
        const double spp = samplesPerPixel();
        const double pointsPerPixel = spp / w.samplesPerPoint;
        const int width = getWidth();

        // Second ticks (no beat grid until Phase 2).
        g.setColour(kGrid);
        const double leftSample = pos - centreX * spp;
        const double firstSecond = std::ceil(leftSample / rate);
        for (double sec = firstSecond;; sec += 1.0)
        {
            const auto x = static_cast<float>((sec * rate - leftSample) / spp);
            if (x > area.getRight())
                break;
            g.fillRect(x, area.getY(), 1.0f, area.getHeight());
        }

        // Bars: each pixel column shows the loudest points under it (or, zoomed in, the point it falls in).
        const int numPoints = w.numPoints();
        for (int x = 0; x < width; ++x)
        {
            const double from = (leftSample + x * spp) / w.samplesPerPoint;
            const auto first = static_cast<int>(std::floor(from));
            const int last = std::max(first, static_cast<int>(std::floor(from + pointsPerPixel)) - 1);
            if (last < 0 || first >= numPoints)
                continue;
            float peaks[3] = {};
            for (int p = std::max(0, first); p <= std::min(last, numPoints - 1); ++p)
                for (int b = 0; b < 3; ++b)
                    peaks[b] = std::max(peaks[b], w.peak[static_cast<std::size_t>(b)][static_cast<std::size_t>(p)]);
            drawBandColumn(g, static_cast<float>(x), 1.0f, mid, half, peaks, x < centreX ? 0.55f : 0.95f);
        }

        // Cue point.
        const auto cueX = static_cast<float>((s.cuePointSamples - leftSample) / spp);
        if (cueX >= area.getX() - 6.0f && cueX <= area.getRight() + 6.0f)
        {
            g.setColour(kCueColour);
            g.fillRect(cueX - 0.5f, area.getY(), 1.5f, area.getHeight());
            juce::Path marker;
            marker.addTriangle(cueX - 6.0f, area.getY(), cueX + 6.0f, area.getY(), cueX, area.getY() + 8.0f);
            g.fillPath(marker);
        }
    }
    else
    {
        g.setColour(juce::Colours::white.withAlpha(0.3f));
        g.setFont(juce::FontOptions(14.0f));
        g.drawText(juce::String("Deck ") + engine::deckLetter(deck) + ": no track", getLocalBounds(),
                   juce::Justification::centred);
    }

    // Playhead and labels.
    g.setColour(juce::Colours::white);
    g.fillRect(centreX - 1.0f, area.getY(), 2.0f, area.getHeight());
    g.setColour(juce::Colours::white.withAlpha(0.6f));
    g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    g.drawText(juce::String::charToString(static_cast<juce::juce_wchar>(engine::deckLetter(deck))),
               getLocalBounds().reduced(6, 4), juce::Justification::topLeft);
    g.setFont(juce::FontOptions(11.0f));
    g.drawText(juce::String(juce::roundToInt(visibleSeconds())) + " s" + (s.scratching ? "  SCRATCH" : ""),
               getLocalBounds().reduced(6, 4), juce::Justification::topRight);

    if (isDropHovering())
        drawDropHighlight(g, area, juce::String("Drop to load deck ") + engine::deckLetter(deck));

    const double ms = juce::Time::getMillisecondCounterHiRes() - started;
    paintSum += ms;
    paintMax = std::max(paintMax, ms);
    if (++paintCount >= kStatsWindow)
    {
        stats.paintMsAverage = paintSum / paintCount;
        stats.paintMsMax = paintMax;
        paintSum = paintMax = 0.0;
        paintCount = 0;
    }
}

// ---- gestures ----

void WaveformView::beginDrag(float x)
{
    dragging = true;
    lastDragX = x;
    engine.send(engine::ScratchBegin{deck, uiClockSeconds()});
}

void WaveformView::dragTo(float x)
{
    if (!dragging)
        return;
    const float dx = x - lastDragX;
    lastDragX = x;
    if (dx != 0.0f) // grab and move: dragging left pulls the track forward, under the hand
        engine.send(engine::ScratchMove{deck, -dx * samplesPerPixel(), uiClockSeconds()});
}

void WaveformView::endDrag()
{
    if (!dragging)
        return;
    dragging = false;
    engine.send(engine::ScratchEnd{deck});
}

void WaveformView::zoomBy(int steps)
{
    zoom = std::clamp(zoom - steps, 0, static_cast<int>(kZoomLevels.size()) - 1);
    repaint();
}

void WaveformView::nudgeNotches(double notches)
{
    const double rate = engine.readState().sampleRate > 0.0 ? engine.readState().sampleRate : 48000.0;
    if (notches != 0.0)
        engine.send(engine::Nudge{deck, notches * kNudgeSecondsPerNotch * rate});
}

void WaveformView::mouseDown(const juce::MouseEvent& e)
{
    beginDrag(e.position.x);
}

void WaveformView::mouseDrag(const juce::MouseEvent& e)
{
    dragTo(e.position.x);
}

void WaveformView::mouseUp(const juce::MouseEvent&)
{
    endDrag();
}

void WaveformView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // JUCE reports about 0.25 per wheel notch; up / right = forward (or zoom in with Ctrl).
    const float amount = std::abs(wheel.deltaY) >= std::abs(wheel.deltaX) ? wheel.deltaY : wheel.deltaX;
    const double notches = (wheel.isReversed ? -amount : amount) / 0.25;
    if (e.mods.isCtrlDown())
        zoomBy(notches > 0.0 ? 1 : notches < 0.0 ? -1 : 0);
    else
        nudgeNotches(notches);
}

} // namespace devildj::ui
