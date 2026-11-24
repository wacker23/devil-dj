#pragma once

#include "dsp/DriftBuffer.h"
#include "dsp/LevelMeter.h"
#include "dsp/Limiter.h"
#include "dsp/fx/ColorFx.h"
#include "dsp/fx/FxUnit.h"
#include "dsp/fx/ReleaseFx.h"
#include "engine/ChannelStrip.h"
#include "engine/Crossfader.h"
#include "engine/MasterClock.h"
#include "engine/Deck.h"
#include "engine/EngineCommand.h"
#include "engine/EngineState.h"
#include "engine/OutputRouter.h"
#include "engine/RecordTap.h"
#include "engine/Sampler.h"
#include "engine/Scheduler.h"
#include "engine/Sequencer.h"
#include "engine/TrackBuffer.h"
#include "engine/Voices.h"
#include "util/SpscQueue.h"
#include "util/TripleBuffer.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

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
    // Mix recording (P3-16): the tap the recorder's writer thread reads (the master after its limiter, or the
    // headphones), filled by the audio thread while enabled.
    RecordTap& recordTap() noexcept { return tap; }

    // FX (P3-08), message thread: prepares the effect for the current device, keeps it, and hands it to the audio
    // thread for that unit / slot (nullptr empties the slot). False if the command queue is full (the effect is
    // then dropped). collectEffects() deletes the effects the audio thread has let go of; call it now and then
    // (the app's timer). Effects are prepared again when the engine is.
    bool installEffect(int unit, int slot, std::unique_ptr<dsp::fx::Effect> effect);
    int collectEffects();
    int ownedEffectCount() const noexcept { return static_cast<int>(ownedEffects.size()); }
    // Pad FX (P3-12), message thread: the effect behind a deck's pad (owned like installEffect's).
    bool installPadEffect(int deck, int pad, std::unique_ptr<dsp::fx::Effect> effect);

    // Latest published state. One reader thread only (the UI); the reference stays valid until the next call.
    const EngineState& readState() noexcept { return state.read(); }

    // A transition with curves (P4-14), message thread: keeps the curves alive (the last few) and sends the command
    // pointing at them. False if the command queue is full.
    bool startTransition(StartTransition command, std::shared_ptr<const TransitionCurves> curves);

    // The Beat Maker's events (P4-10): its producer thread (only that one) pushes them; the audio thread plays
    // them at their samples on the BEATS channel.
    EventQueue& beatMakerEvents() noexcept { return beatMaker->queue; }
    EventQueue& codeDeckEvents() noexcept { return codeDeck->queue; } // P5-12: the code deck's producer -> here

    // The master clock (P2-18), for code on the audio thread (sync, quantize, the scheduler).
    const MasterClock& masterClock() const noexcept { return clock; }

    // P2-25: with the SeparateCueDevice layout the cue bus goes here, for the headphone device's callback
    // (engine/CueDevice) to read on its own thread.
    dsp::DriftBuffer& separateCueOutput() noexcept { return cueOutput; }

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
    void apply(const SetKeyLock& command) noexcept;
    void apply(const SetKeyShift& command) noexcept;
    void apply(const ScratchBegin& command) noexcept;
    void apply(const ScratchMove& command) noexcept;
    void apply(const ScratchEnd& command) noexcept;
    void apply(const Nudge& command) noexcept;
    void apply(const SetParam& command) noexcept;
    void apply(const SetBeatgrid& command) noexcept;
    void apply(const SetMetronome& command) noexcept;
    void apply(const SetClockSource& command) noexcept;
    void apply(const SetInternalTempo& command) noexcept;
    void apply(const SetSync& command) noexcept;
    void apply(const SetQuantize& command) noexcept;
    void apply(const BeatJump& command) noexcept;
    void apply(const JumpBy& command) noexcept;
    void apply(const SetHotCues& command) noexcept;
    void apply(const SetCuePoint& command) noexcept;
    void apply(const Loop& command) noexcept;
    void apply(const HotCue& command) noexcept;
    void apply(const SetSlip& command) noexcept;
    void apply(const SetReverse& command) noexcept;
    void apply(const Stop& command) noexcept;
    void apply(const SetStopStyle& command) noexcept;
    void apply(const SetSlicer& command) noexcept;
    void apply(const SlicerPad& command) noexcept;
    void apply(const FxSetEffect& command) noexcept;
    void apply(const FxCommand& command) noexcept;
    void apply(const SetColorFx& command) noexcept;
    void apply(const ReleaseFxCommand& command) noexcept;
    void apply(const PadFxSetEffect& command) noexcept;
    void apply(const PadFxCommand& command) noexcept;
    void apply(const SamplerLoad& command) noexcept;
    void apply(const SamplerPad& command) noexcept;
    void apply(const SamplerSet& command) noexcept;
    void apply(const SequencerCommand& command) noexcept;
    void apply(const SequencerEvents& command) noexcept;
    void apply(const StartTransition& command) noexcept;
    void apply(const CancelTransition& command) noexcept;
    void apply(const SetDrumSound& command) noexcept;
    void apply(const SetSynthSound& command) noexcept;
    void apply(const BeatMakerStop& command) noexcept;
    void apply(const CodeDeckStop& command) noexcept;
    void apply(const SetSampleSound& command) noexcept;
    void apply(const TransitionFxSetEffect& command) noexcept;
    void runTransition() noexcept; // the automation lanes, once a block (after the clock)
    void endTransition(bool finished) noexcept;
    void setLowEq(DeckId deck, bool kill) noexcept;
    void renderSequencerClick(int numSamples) noexcept;
    // One channel from its source's audio in l / r to the master and cue buses (strip, color FX, pad FX, release FX,
    // the FX units on it, meter, cue, fader, crossfader).
    void mixChannel(int channel, float* l, float* r, int numSamples) noexcept;
    void renderSampler(int numSamples) noexcept;
    struct VoiceModule;
    void renderVoices(VoiceModule& module, int channel, int numSamples) noexcept; // the Beat Maker's, the code deck's
    VoiceModule& moduleOf(SourceId source) noexcept;
    dsp::fx::BeatContext beatContext() const noexcept;
    void processFx(FxTarget target, float* left, float* right, int numSamples) noexcept;
    void retireEffect(dsp::fx::Effect* effect) noexcept;
    void followMaster() noexcept;                              // P2-19: every synced deck that doesn't lead
    bool tempoToMaster(DeckId deck, double percent) noexcept; // sync lock: a follower's fader moves the master
    void advanceClock(int numSamples) noexcept;
    // Metronome clicks for the beats the deck crossed in this chunk, into the cue bus (P2-14).
    void renderMetronome(std::size_t deck, double before, double after, int numSamples) noexcept;
    void requestTrack(DeckId deck, const TrackBuffer* buffer, BufferId id) noexcept;
    void swapPendingTracks() noexcept;
    void renderDecks(int numSamples) noexcept;
    void releaseBuffer(BufferId id) noexcept;
    void retryPendingReleases() noexcept;
    void renderChunk(float* const* outputs, int numChannels, int offset, int numSamples) noexcept;
    void renderTestTone(int numSamples) noexcept;
    void publish(int numChannels, int numSamples, double elapsedSeconds) noexcept;

    // On the heap: a command is ~0.5 KB since grids travel in them (P2-14), 1,024 of them don't belong on a stack.
    using CommandQueue = util::SpscQueue<EngineCommand, kCommandQueueSize>;
    std::unique_ptr<CommandQueue> commandQueue = std::make_unique<CommandQueue>();
    CommandQueue& commands = *commandQueue;
    util::SpscQueue<BufferId, 256> releasedBuffers; // audio thread -> message thread
    // FX units (P3-08): the units and their targets (audio thread), the effects (owned by the message thread).
    std::array<dsp::fx::FxUnit, kFxUnits> fxUnits;
    std::array<FxTarget, kFxUnits> fxTargets{FxTarget::Master, FxTarget::Master};
    std::array<std::array<float, kFxSlots>, kFxUnits> fxBeats{};
    std::array<double, kFxUnits> fxManualBpm{}, fxManualBeat{}; // P3-09 tap tempo
    util::SpscQueue<dsp::fx::Effect*, 64> releasedEffects; // audio thread -> message thread
    static constexpr int kMaxPendingEffects = 16;
    std::array<dsp::fx::Effect*, kMaxPendingEffects> pendingEffects{};
    int numPendingEffects = 0;
    std::vector<std::unique_ptr<dsp::fx::Effect>> ownedEffects; // message thread only
    int preparedBlockSize = 512;
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
        util::Beatgrid grid; // P2-14, in this deck's buffer samples
        bool synced = false;      // P2-19
        bool snapPending = false; // jump into phase once (sync pressed, play started) if far off
        double phaseErrorMs = 0.0;
        // Quantized actions waiting for their grid point (P2-20; ARCHITECTURE §3.4), in order.
        struct QueuedAction
        {
            enum class Kind : std::uint8_t
            {
                Play, // at master beat `at`
                Jump, // when the playhead crosses buffer sample `at`: to `to` (+ the overshoot)
            };
            Kind kind = Kind::Play;
            double at = 0.0, to = 0.0;
            double loopLength = 0.0; // Jump to a hot loop: loop from `to` for this long once there (P3-03)
        };
        static constexpr int kMaxQueued = 16;
        std::array<QueuedAction, kMaxQueued> queued{};
        int queuedCount = 0;
        std::array<HotCueSlot, kHotCues> hotCues{}; // P3-01, in buffer samples
        int hotCuePreview = -1;                     // held from pause
        int slipHoldPad = -1;                       // P3-04: held while playing with slip on (returns on release)
        // Slicer (P3-07): the domain in grid beats, the pads held, the slice rolling.
        bool slicerOn = false;
        double slicerBeats = 8.0;
        double slicerStartBeat = 0.0;
        std::uint8_t slicerHeld = 0;
        int slicerRolling = -1;
        // Loops (P3-03).
        double autoLoopBeats = 4.0;
        bool loopInPending = false;
        double loopInPoint = 0.0;
        bool haveLastLoop = false; // for reloop
        double lastLoopStart = 0.0, lastLoopEnd = 0.0;
        bool metronome = false;
        float metronomeGain = 0.5f;
        // The click being played: a short decaying sine, continued across chunks.
        int clickDelay = -1;   // samples into the chunk where it starts; -1 = none starting
        int clickPosition = 0; // samples of it played so far
        int clickLength = 0;
        double clickIncrement = 0.0;
        float clickLevel = 0.0f;
        bool clickActive = false;
    };
    std::array<DeckSlot, kMaxDecks> decks{};
    // How TogglePlay stops a playing deck, and the brake / spin-back times (P3-05).
    StopKind pauseStyle = StopKind::Instant;
    double brakeSeconds = 1.0, spinBackSeconds = 1.0;
    juce::AudioBuffer<float> deckScratch;            // 2 x capacity, one deck at a time
    std::array<ChannelStrip, kMaxChannels> strips{}; // channel i = deck i
    // Color FX (P3-10): one per channel, after the strip's filter; the FILTER knob drives the strip's filter
    // or, with another character chosen, the color FX (the strip's filter then stays at its centre).
    std::array<dsp::fx::ColorFx, kMaxChannels> colorFx;
    std::array<double, kMaxChannels> filterKnob{0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5};
    // The sampler (P3-13) and its quantized presses waiting for their beat.
    Sampler sampler;
    // A voice module, on the heap (its voices and queues are big): the voices, the events from its producer, the
    // events due, and its own echo / reverb for the sends. The Beat Maker's (P4-10) and the code deck's (P5-12).
    struct VoiceModule
    {
        Voices voices;
        EventQueue queue;
        EventBuffer<2048> due;
        juce::AudioBuffer<float> scratch; // 6 x capacity: dry L / R, delay send L / R, reverb send L / R
        std::unique_ptr<dsp::fx::Effect> delay, reverb;
        std::uint64_t late = 0;
    };
    std::unique_ptr<VoiceModule> beatMaker = std::make_unique<VoiceModule>();
    std::unique_ptr<VoiceModule> codeDeck = std::make_unique<VoiceModule>();
    std::array<double, Sampler::kSlots> samplerPendingBeat{}; // < 0 = none
    bool samplerPending = false;
    // The sequencer (P3-15) and its metronome click (into the headphones while recording).
    Sequencer sequencer;
    RecordTap tap; // P3-16
    // The auto-transition (P3-17).
    struct Transition
    {
        bool active = false, started = false, swapped = false, echoPressed = false, faderMode = false;
        DeckId from = 0, to = 1;
        double beats = 16.0, startBeat = 0.0;
        TransitionStyle style = TransitionStyle::Blend;
        double crossfaderFrom = 0.0, crossfaderTo = 1.0, fromFader = 1.0;
        // P4-14: curves, and what the two channels were before (put back at the end).
        const TransitionCurves* curves = nullptr;
        struct Saved
        {
            std::array<double, 3> eq{};
            double filter = 0.5;
            CrossfaderAssign assign = CrossfaderAssign::Thru;
        };
        std::array<Saved, 2> saved{}; // [0] the outgoing channel, [1] the incoming one
        std::array<float, 12> lanes{};
        std::uint16_t lanesUsed = 0;
        // The outgoing deck's crossfader gain when it began: its lanes are scaled by it, so taking both decks off
        // the crossfader (THRU) doesn't make it jump.
        float fromScale = 1.0f;
    };
    Transition transition;
    void runCurves() noexcept; // P4-14: one block of a curve transition
    // P4-18: the transition's two send effects, what was sent into them this block (4 x capacity: side 0 L / R,
    // side 1 L / R) and how long their tails still run; the loop out.
    std::array<dsp::fx::Effect*, 2> transitionFx{};
    juce::AudioBuffer<float> transitionFxBus;
    std::array<bool, 2> transitionFxFed{};
    std::array<int, 2> transitionFxTail{};
    bool transitionLoop = false;
    void processTransitionFx(int numSamples) noexcept;
    std::array<std::shared_ptr<const TransitionCurves>, 4> heldCurves;
    int heldNext = 0;
    bool tapPrepared = false;
    bool sequencerMetronome = false;
    int seqClickPosition = 0, seqClickLength = 0, seqClickDelay = -1;
    double seqClickIncrement = 0.0;
    float seqClickLevel = 0.0f;
    bool seqClickActive = false;
    dsp::fx::ColorFx::Type colorType = dsp::fx::ColorFx::Type::Filter;
    // Release FX (P3-11), one per channel after the color FX; when one has ended its deck pauses.
    std::array<dsp::fx::ReleaseFx, kMaxChannels> releaseFx;
    // Pad FX (P3-12): per channel, one slot per pad, after the color FX (before the release FX).
    std::array<std::array<dsp::fx::FxSlot, kPadFxSlots>, kMaxChannels> padFx;
    struct MixerChannel
    {
        CrossfaderAssign assign = CrossfaderAssign::Thru;
        dsp::LinearRamp crossfaderGain; // this channel's side of the crossfader, 20 ms
        dsp::LevelMeter meter;          // pre-fader
        bool cue = false;
        dsp::LinearRamp cueSend; // 0/1, 20 ms: pre-fader signal into the cue bus
        // P4-14: a transition's volume lane - a gain after the fader, from its value at the block's start to the
        // value at its end (sample-accurate along the curve).
        bool autoGain = false;
        float autoGainStart = 1.0f, autoGainEnd = 1.0f;
        // P4-18: a transition's FX lane - this channel (after its volume lane) x the lane into side 0 / 1's effect.
        int transitionSend = -1;
        float sendStart = 0.0f, sendEnd = 0.0f;
    };
    std::array<MixerChannel, kMaxChannels> mixer{};
    double crossfaderPosition = 0.5;
    CrossfaderCurve crossfaderCurve = CrossfaderCurve::Smooth;
    double masterGainDb = 0.0;
    dsp::LinearRamp masterGain;
    dsp::Limiter limiter;
    dsp::LevelMeter masterMeter;
    double cueMix = 0.5, headphoneLevelDb = 0.0;
    dsp::LinearRamp cueMixCue, cueMixMaster, headphoneGain;
    dsp::Limiter cueLimiter; // the headphones can't clip either, and stay aligned with the limited master
    dsp::LevelMeter cueMeter;
    dsp::DriftBuffer cueOutput; // P2-25
    MasterClock clock;
    bool quantizeOn = false;
    double quantizeBeats = 1.0;
    void startPlaying(DeckSlot& slot) noexcept;
    void queue(DeckSlot& slot, DeckSlot::QueuedAction action) noexcept;
    void updateSlicer(DeckSlot& slot) noexcept;          // domain shift (P3-07)
    void rollSlice(DeckSlot& slot, int pad) noexcept;    // the slice's loop from its next boundary
    void dropQueuedJumps(DeckSlot& slot) noexcept;
    // Renders one deck, split at the samples where its queued actions fall due.
    void renderDeckWithActions(DeckSlot& slot, float* l, float* r, int numSamples) noexcept;
    void updateCueMix() noexcept;
    void mixHeadphones(int numSamples) noexcept;
    void finishBuses(int numSamples) noexcept;
    float blockGainReductionDb = 0.0f;
    bool mixerInitialised = false;
    void updateCrossfaderGains() noexcept;
    void renderMaster(int numSamples) noexcept;

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
