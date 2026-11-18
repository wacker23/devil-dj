#pragma once

#include "engine/Deck.h"
#include "engine/EngineCommand.h"
#include "engine/EngineState.h"
#include "engine/OutputRouter.h"
#include "engine/TrackBuffer.h"
#include "util/SpscQueue.h"
#include "util/TripleBuffer.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace devildj::engine
{

// The real-time audio engine (ARCHITECTURE §3.1). P1-02 skeleton + P1-03 output routing.
//
// Threads:
//   - Message thread: send() commands, readState() for the UI.
//   - Audio thread: audioDeviceIOCallbackWithContext() (or process() in tests). It never allocates, locks,
//     logs or blocks; RealtimeGuard checks this in Debug builds.
//
// Per block: drain up to kMaxCommandsPerBlock commands; render the master and cue buses (for now only the
// test tone); route them to the device outputs for the current OutputLayout; measure CPU; publish EngineState.
// Blocks larger than the size given to prepare() are processed in chunks.
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

    // Ids of buffers the engine no longer uses; the owner pops them (10 Hz housekeeping) and releases them from
    // the BufferRegistry. Message thread.
    bool popReleasedBuffer(BufferId& id) noexcept { return releasedBuffers.tryPop(id); }

    // Latest published state. One reader thread only (the UI); the reference stays valid until the next call.
    const EngineState& readState() noexcept { return state.read(); }

    // ---- not on the audio thread: allocates the bus buffers ----

    // The device callback calls this from audioDeviceAboutToStart. Call it yourself for offline processing.
    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;

    // ---- audio thread ----

    // Renders one block into 'outputs' (numChannels pointers to numSamples floats; null pointers are skipped).
    void process(float* const* outputs, int numChannels, int numSamples) noexcept;

    // juce::AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs, float* const* outputs,
                                          int numOutputs, int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    static constexpr int kMaxOutputChannels = 32;

    void drainCommands() noexcept;
    void apply(const SetTestTone& command) noexcept;
    void apply(const SetOutputLayout& command) noexcept;
    void apply(const LoadTrack& command) noexcept;
    void apply(const UnloadTrack& command) noexcept;
    void apply(const Play& command) noexcept;
    void apply(const Pause& command) noexcept;
    void apply(const TogglePlay& command) noexcept;
    void apply(const CuePress& command) noexcept;
    void apply(const CueRelease& command) noexcept;
    void apply(const Seek& command) noexcept;
    void apply(const SetTempo& command) noexcept;
    void apply(const NudgeTempo& command) noexcept;
    void apply(const SetTempoRange& command) noexcept;
    void apply(const ResetTempo& command) noexcept;
    void apply(const PitchBend& command) noexcept;
    void apply(const SetInterpolationQuality& command) noexcept;
    void requestTrack(DeckId deck, const TrackBuffer* buffer, BufferId id) noexcept;
    void swapPendingTracks() noexcept;
    void renderDecks(int numSamples) noexcept;
    void releaseBuffer(BufferId id) noexcept;
    void retryPendingReleases() noexcept;
    void renderChunk(float* const* outputs, int numChannels, int offset, int numSamples) noexcept;
    void renderTestTone(int numSamples) noexcept;
    void publish(int numChannels, int numSamples, double elapsedSeconds) noexcept;

    util::SpscQueue<EngineCommand, kCommandQueueSize> commands;
    util::SpscQueue<BufferId, 256> releasedBuffers; // audio thread -> message thread
    util::TripleBuffer<EngineState> state;
    std::atomic<std::uint64_t> dropped{0};

    // Audio-thread state.
    double sampleRate = 48000.0;
    int capacity = 0;                   // samples per chunk (prepared block size)
    juce::AudioBuffer<float> masterBus; // 2 x capacity
    juce::AudioBuffer<float> cueBus;    // 2 x capacity
    OutputLayout layout = OutputLayout::StereoMaster;

    // A deck plus the registry id of its buffer. A new track for a deck that is still sounding waits in
    // `pending` until the deck has faded out (2 ms), so the old buffer is never released while being read.
    struct DeckSlot
    {
        Deck deck;
        BufferId id = 0;
        bool hasPending = false;
        const TrackBuffer* pendingBuffer = nullptr;
        BufferId pendingId = 0;
    };
    std::array<DeckSlot, kMaxDecks> decks{};
    juce::AudioBuffer<float> deckScratch; // 2 x capacity, one deck at a time

    // Releases that didn't fit into releasedBuffers yet; retried every block.
    static constexpr int kMaxPendingReleases = 16;
    std::array<BufferId, kMaxPendingReleases> pendingReleases{};
    int numPendingReleases = 0;

    EngineState current; // built up during the block, then published

    struct TestTone
    {
        bool on = false;
        double phase = 0.0;          // radians
        double phaseIncrement = 0.0; // radians per sample
        float frequencyHz = 440.0f;
        Bus bus = Bus::Master;
        float gainLeft = 0.70710678f; // constant-power pan gains
        float gainRight = 0.70710678f;
        juce::SmoothedValue<float> gain; // linear, ramped over 20 ms
    } tone;
};

} // namespace devildj::engine
