#pragma once

#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>

namespace devildj::ui
{

// Zoomed waveform of one deck (P1-14), centred on the playhead, 3-band colours.
//   drag             scratch while playing / scrub while paused (P1-09): the waveform follows the hand
//   wheel            fine nudge (5 ms per notch)
//   Ctrl + wheel     zoom (2 ... 32 s visible)
// Repaints from VBlankAttachment. Between engine states (one per audio block) the playhead is extrapolated
// from the state's timestamp and rate, then moved back by the output latency so the line is where the sound
// is heard. Paint time and frame interval are measured for the 60 fps target.
class WaveformView final : public juce::Component
{
public:
    using TrackLookup = std::function<std::shared_ptr<const engine::TrackBuffer>(engine::BufferId)>;
    using LatencySource = std::function<double()>; // seconds from "audio block done" to "heard"

    WaveformView(engine::AudioEngine& engine, engine::DeckId deck, TrackLookup lookup, LatencySource latency);

    static constexpr std::array<double, 7> kZoomLevels{2.0, 4.0, 6.0, 8.0, 12.0, 16.0, 32.0}; // seconds visible
    static constexpr int kDefaultZoom = 2;                                                    // 6 s
    static constexpr double kNudgeSecondsPerNotch = 0.005;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    // The gesture paths the mouse handlers use (public for tests).
    void beginDrag(float x);
    void dragTo(float x);
    void endDrag();
    void zoomBy(int steps); // + = zoom in (fewer seconds)
    void nudgeNotches(double notches);

    double visibleSeconds() const noexcept { return kZoomLevels[static_cast<std::size_t>(zoom)]; }
    double samplesPerPixel() const noexcept;

    // Playhead to draw: state position + rate x time since the state, minus rate x output latency.
    static double displayedPosition(const engine::DeckState& state, double sampleRate, double stateTime, double now,
                                    double latencySeconds) noexcept;

    struct FrameStats
    {
        double paintMsAverage = 0.0, paintMsMax = 0.0; // over the last ~2 s
        double frameIntervalMs = 0.0;                  // average time between repaints
    };
    FrameStats frameStats() const noexcept { return stats; }

private:
    void onVBlank();
    static double uiClockSeconds();

    engine::AudioEngine& engine;
    engine::DeckId deck;
    TrackLookup lookup;
    LatencySource latency;
    engine::BufferId shownBuffer = 0;
    std::shared_ptr<const engine::TrackBuffer> track;
    int zoom = kDefaultZoom;
    float lastDragX = 0.0f;
    bool dragging = false;
    double cachedLatency = 0.0;
    int framesSinceLatency = 1000;

    FrameStats stats;
    double lastVBlank = 0.0;
    double paintSum = 0.0, paintMax = 0.0, intervalSum = 0.0;
    int paintCount = 0, intervalCount = 0;

    juce::VBlankAttachment vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformView)
};

} // namespace devildj::ui
