#pragma once

#include "engine/EngineCommand.h"
#include "engine/EngineState.h"
#include "util/SpscQueue.h"
#include "util/TripleBuffer.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cstdint>

namespace devildj::engine
{

// The real-time audio engine (P1-02 skeleton; ARCHITECTURE §3.1).
//
// Threads:
//   - Message thread: send() commands, readState() for the UI.
//   - Audio thread: audioDeviceIOCallbackWithContext() (or process() in tests). It never allocates, locks,
//     logs or blocks; RealtimeGuard checks this in Debug builds.
//
// Per block: drain up to kMaxCommandsPerBlock commands, render (for now: silence or the test tone on every
// output channel), measure the CPU load, publish EngineState.
class AudioEngine final : public juce::AudioIODeviceCallback
{
public:
    static constexpr std::size_t kCommandQueueSize = 1024;
    static constexpr int kMaxCommandsPerBlock = 256;

    AudioEngine() = default;
    ~AudioEngine() override = default;

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // ---- message thread ----

    // Queues a command for the next block. Returns false (and counts it) if the queue is full.
    bool send(const EngineCommand& command) noexcept;
    std::uint64_t droppedCommands() const noexcept { return dropped.load(std::memory_order_relaxed); }

    // Latest published state. One reader thread only (the UI); the reference stays valid until the next call.
    const EngineState& readState() noexcept { return state.read(); }

    // ---- audio thread (also callable directly, e.g. offline tests) ----

    // Call before processing (the device callback does this in audioDeviceAboutToStart).
    void prepare(double sampleRate, int maxBlockSize) noexcept;
    void reset() noexcept;

    // Renders one block into 'outputs' (numChannels pointers to numSamples floats; null pointers are skipped).
    void process(float* const* outputs, int numChannels, int numSamples) noexcept;

    // juce::AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs, float* const* outputs,
                                          int numOutputs, int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    void drainCommands() noexcept;
    void apply(const SetTestTone& command) noexcept;
    void renderTestTone(float* const* outputs, int numChannels, int numSamples) noexcept;
    void publish(int numSamples, double elapsedSeconds) noexcept;

    util::SpscQueue<EngineCommand, kCommandQueueSize> commands;
    util::TripleBuffer<EngineState> state;
    std::atomic<std::uint64_t> dropped{0};

    // Audio-thread state.
    double sampleRate = 48000.0;
    int maxBlockSize = 0;
    EngineState current; // built up during the block, then published

    struct TestTone
    {
        bool on = false;
        double phase = 0.0;          // radians
        double phaseIncrement = 0.0; // radians per sample
        float frequencyHz = 440.0f;
        juce::SmoothedValue<float> gain; // linear, ramped over 20 ms
    } tone;
};

} // namespace devildj::engine
