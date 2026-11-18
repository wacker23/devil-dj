#pragma once

#include "engine/AudioDeviceService.h"
#include "engine/EngineCommand.h"
#include "engine/EngineState.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <vector>

namespace devildj::app
{

// Soak test (P1-20): plays decks A and B for a long time the way a set would stress the engine, and records
// whether the audio ever glitched.
//   - key lock on both decks, tempo +3 % / -2 %, both playing; each deck jumps back when it nears the end
//     (no loops until Phase 2), so they never stop
//   - EQ low / mid / high, filter and crossfader keep moving (slow sines, different speeds per control)
//   - every minute: engine xruns (blocks over budget), driver xruns, audio CPU mean / peak, process memory
//   - at the end: a text report (verdict, settings, the per-minute table) and the decks are reset
// Silent mode mutes the master (everything is still processed) so it can run without disturbing anyone.
// Message thread only. tick() is driven by a 10 Hz timer in the app, or by hand in tests.
class SoakTest final : private juce::Timer
{
public:
    struct Hooks
    {
        std::function<engine::EngineState()> engineState;
        std::function<engine::AudioDeviceStatus()> deviceStatus;
        std::function<void(const engine::EngineCommand&)> send;
        std::function<double()> memoryMb; // process private memory
        std::function<double()> now;      // seconds (steady clock)
    };
    struct Options
    {
        double minutes = 60.0;
        bool silent = false;
    };
    struct Minute
    {
        int index = 0;
        std::uint32_t engineXruns = 0, deviceXruns = 0; // during this minute
        float cpuMean = 0.0f, cpuPeak = 0.0f;           // 0..1 of the block budget
        double memoryMb = 0.0;
    };

    explicit SoakTest(Hooks hooks);
    ~SoakTest() override;

    // Returns an error (e.g. "Load a track on deck A and deck B first"), or empty when started.
    juce::String start(Options options, bool useTimer = true);
    void stop(); // finishes early (still reports)
    bool isRunning() const noexcept { return running; }
    void tick();

    double elapsedMinutes() const;
    std::uint32_t totalXruns() const noexcept;
    const std::vector<Minute>& minutes() const noexcept { return perMinute; }
    juce::String statusLine() const; // for the overlay
    juce::String report() const;     // after it finished
    bool passed() const noexcept { return totalXruns() == 0; }

    std::function<void(const juce::String& report)> onFinished;

    // Next bigger buffer to try after a failed run (power of two steps from the current one).
    static int suggestedBuffer(int currentBuffer) noexcept;

private:
    void timerCallback() override { tick(); }
    void finish();
    void automate(double t, const engine::EngineState& s);
    void closeMinute(double now, const engine::EngineState& s, const engine::AudioDeviceStatus& d);

    Hooks hooks;
    Options options;
    bool running = false;
    double startTime = 0.0, minuteStart = 0.0, endTime = 0.0;
    std::uint32_t engineXrunsAtMinute = 0;
    int deviceXrunsAtMinute = 0;
    double cpuSum = 0.0;
    int cpuSamples = 0;
    float cpuPeak = 0.0f;
    std::vector<Minute> perMinute;
    engine::AudioDeviceStatus deviceAtStart;
    juce::String startedAt;
};

} // namespace devildj::app
