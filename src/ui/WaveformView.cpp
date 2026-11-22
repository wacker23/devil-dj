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

    if (preview != nullptr && s.bufferId != previewOver)
        preview.reset(); // the loaded track is on the deck now
    const analysis::Waveform* shown = preview != nullptr                              ? preview.get()
                                      : track != nullptr && !track->waveform.empty() ? &track->waveform
                                                                                     : nullptr;
    if (shown != nullptr)
    {
        const auto& w = *shown;
        const double latencySeconds = cachedLatency + es.masterLatencySamples / rate;
        const double pos =
            preview != nullptr ? 0.0 : displayedPosition(s, rate, es.hostTimeSeconds, uiClockSeconds(), latencySeconds);
        const double spp = samplesPerPixel();
        const double pointsPerPixel = spp / w.samplesPerPoint;
        const int width = getWidth();

        // Beat lines from the grid (P2-14; downbeats brighter, bar numbers on top), else second ticks.
        const double leftSample = pos - centreX * spp;
        const auto grid = gridSource && preview == nullptr ? gridSource() : util::Beatgrid();
        if (!grid.empty())
        {
            const double rightSample = leftSample + width * spp;
            const auto firstBeat = static_cast<long long>(std::floor(grid.beatAt(leftSample)));
            g.setFont(juce::FontOptions(10.0f));
            for (long long b = firstBeat;; ++b)
            {
                const double at = grid.sampleAt(static_cast<double>(b));
                if (at > rightSample)
                    break;
                const auto x = static_cast<float>((at - leftSample) / spp);
                const bool downbeat = grid.barBeatOf(b) == 1;
                g.setColour(downbeat ? juce::Colour(0xffe0483c).withAlpha(0.8f) : juce::Colours::white.withAlpha(0.18f));
                g.fillRect(x, area.getY(), downbeat ? 1.5f : 1.0f, area.getHeight());
            }
        }
        else
        {
            g.setColour(kGrid);
            const double firstSecond = std::ceil(leftSample / rate);
            for (double sec = firstSecond;; sec += 1.0)
            {
                const auto x = static_cast<float>((sec * rate - leftSample) / spp);
                if (x > area.getRight())
                    break;
                g.fillRect(x, area.getY(), 1.0f, area.getHeight());
            }
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

        // Hot cues (P3-01): a line and a flag with the letter.
        if (preview == nullptr)
        {
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            for (int c = 0; c < 16; ++c)
            {
                const double at = s.hotCueStart[static_cast<std::size_t>(c)];
                if (at < 0.0)
                    continue;
                const auto x = static_cast<float>((at - leftSample) / spp);
                if (x < area.getX() - 16.0f || x > area.getRight() + 2.0f)
                    continue;
                const auto colour = juce::Colour(0xff000000u | hotCueColours[static_cast<std::size_t>(c)]);
                g.setColour(colour);
                g.fillRect(x - 0.5f, area.getY(), 1.5f, area.getHeight());
                const juce::Rectangle<float> flag(x, area.getBottom() - 15.0f, 14.0f, 15.0f);
                g.fillRect(flag);
                g.setColour(juce::Colours::black);
                g.drawText(juce::String::charToString(static_cast<juce::juce_wchar>('A' + c)), flag,
                           juce::Justification::centred);
            }
        }

        // Cue point (the loading track's comes with it).
        const auto cueX = static_cast<float>((s.cuePointSamples - leftSample) / spp);
        if (preview == nullptr && cueX >= area.getX() - 6.0f && cueX <= area.getRight() + 6.0f)
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

void WaveformView::showPreview(std::shared_ptr<const analysis::Waveform> waveform)
{
    preview = std::move(waveform);
    previewOver = engine.readState().decks[deck].bufferId;
    repaint();
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
