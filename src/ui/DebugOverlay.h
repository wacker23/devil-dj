#pragma once

#include "engine/AudioDeviceService.h"
#include "engine/EngineState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>

namespace devildj::ui
{

// Developer overlay (toggle with F12; P1-02, extended in P1-20): audio CPU meter with peak, block counter,
// xruns, sample rate / buffer and test tone state. Translucent, ignores the mouse.
//
// It never holds engine pointers (ARCHITECTURE §9): it pulls snapshots through the two functions it is given.
// Repaints from juce::VBlankAttachment at display rate; the text updates at most 10x per second.
class DebugOverlay final : public juce::Component
{
public:
    using EngineStateSource = std::function<engine::EngineState()>;
    using DeviceStatusSource = std::function<engine::AudioDeviceStatus()>;

    using ExtraLinesSource = std::function<juce::StringArray()>;

    DebugOverlay(EngineStateSource engineState, DeviceStatusSource deviceStatus);

    // More lines under the built-in ones (e.g. deck and loader status). Read at most 10x per second.
    void setExtraLines(ExtraLinesSource source) { readExtra = std::move(source); }

    void paint(juce::Graphics& g) override;

    static constexpr int kWidth = 430;
    static constexpr int kHeight = 360;

private:
    void onVBlank(double timestampSeconds);

    EngineStateSource readEngine;
    DeviceStatusSource readDevice;
    ExtraLinesSource readExtra;
    juce::StringArray extraLines;
    juce::VBlankAttachment vblank;

    engine::EngineState engineState;
    engine::AudioDeviceStatus deviceStatus;
    double lastTextUpdate = 0.0;
};

} // namespace devildj::ui
