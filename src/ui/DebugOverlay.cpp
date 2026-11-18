#include "ui/DebugOverlay.h"

#include "util/RealtimeGuard.h"

namespace devildj::ui
{

namespace
{
constexpr double kTextIntervalSeconds = 0.1;

juce::Colour loadColour(float load)
{
    if (load < 0.5f)
        return juce::Colour(0xff3ddc84); // green
    if (load < 0.8f)
        return juce::Colour(0xffffc94d); // amber
    return juce::Colour(0xffff5b5b);     // red
}
} // namespace

DebugOverlay::DebugOverlay(EngineStateSource engineSource, DeviceStatusSource deviceSource)
    : readEngine(std::move(engineSource))
    , readDevice(std::move(deviceSource))
    , vblank(this, [this](double t) { onVBlank(t); })
{
    setInterceptsMouseClicks(false, false);
    setSize(kWidth, kHeight);
}

void DebugOverlay::onVBlank(double timestampSeconds)
{
    if (!isShowing())
        return;

    engineState = readEngine();
    if (timestampSeconds - lastTextUpdate >= kTextIntervalSeconds || lastTextUpdate == 0.0)
    {
        deviceStatus = readDevice();
        lastTextUpdate = timestampSeconds;
    }
    repaint();
}

void DebugOverlay::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    g.setColour(juce::Colours::black.withAlpha(0.72f));
    g.fillRoundedRectangle(area, 6.0f);

    auto content = getLocalBounds().reduced(10, 8);
    g.setFont(juce::FontOptions(13.0f));

    // CPU meter: bar = smoothed load, tick = decaying peak.
    const auto& s = engineState;
    auto meterRow = content.removeFromTop(18);
    g.setColour(juce::Colours::white.withAlpha(0.85f));
    g.drawText("Audio CPU", meterRow.removeFromLeft(72), juce::Justification::centredLeft);
    auto value = meterRow.removeFromRight(92);
    g.drawText(juce::String(100.0f * s.cpuLoad, 1) + " %  pk " +
                   juce::String(juce::roundToInt(100.0f * s.cpuLoadPeak)) + " %",
               value, juce::Justification::centredRight);

    auto bar = meterRow.reduced(4, 4).toFloat();
    g.setColour(juce::Colours::white.withAlpha(0.15f));
    g.fillRect(bar);
    g.setColour(loadColour(s.cpuLoad));
    g.fillRect(bar.withWidth(bar.getWidth() * juce::jlimit(0.0f, 1.0f, s.cpuLoad)));
    g.setColour(juce::Colours::white);
    const float peakX = bar.getX() + bar.getWidth() * juce::jlimit(0.0f, 1.0f, s.cpuLoadPeak);
    g.drawVerticalLine(juce::roundToInt(peakX), bar.getY(), bar.getBottom());

    content.removeFromTop(6);
    const auto& d = deviceStatus;
    juce::StringArray lines;
    lines.add(d.isOpen ? juce::String(juce::roundToInt(d.sampleRate)) + " Hz, " + juce::String(d.bufferSize) +
                             " samples, " + juce::String(d.totalOutputLatencyMs, 1) + " ms"
                       : juce::String("No audio device"));
    lines.add("Blocks " + juce::String(static_cast<juce::int64>(s.blockCounter)) + "   engine xruns " +
              juce::String(static_cast<int>(s.xruns)) + "   device xruns " + juce::String(d.xruns));
    lines.add("Commands " + juce::String(static_cast<juce::int64>(s.commandsProcessed)) + "   test tone " +
              (s.testToneOn ? juce::String(juce::roundToInt(s.testToneHz)) + " Hz" : juce::String("off")));
    if (util::kRealtimeGuardEnabled)
        lines.add("RealtimeGuard violations: " +
                  juce::String(static_cast<juce::int64>(util::realtimeViolationCount())));
    lines.add("F12: hide");

    g.setColour(juce::Colours::white.withAlpha(0.85f));
    for (const auto& line : lines)
        g.drawText(line, content.removeFromTop(18), juce::Justification::centredLeft);
}

} // namespace devildj::ui
