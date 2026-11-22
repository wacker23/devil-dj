#include "engine/AudioEngine.h"

#include "util/RealtimeGuard.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace devildj::engine
{

namespace
{
constexpr double kGainRampSeconds = 0.020;
constexpr float kLoadSmoothing = 0.1f; // one-pole coefficient per block
constexpr float kPeakDecay = 0.995f;   // per block (~4 s to halve at 512 samples / 48 kHz)
} // namespace

bool AudioEngine::send(const EngineCommand& command) noexcept
{
    if (commands.tryPush(command))
        return true;
    dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void AudioEngine::prepare(double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    capacity = std::max(maxBlockSize, 1);
    preparedBlockSize = capacity;
    for (auto& unit : fxUnits) // P3-08
        unit.prepare(sampleRate, capacity);
    for (auto& effect : ownedEffects)
        effect->prepare(sampleRate, capacity);
    masterBus.setSize(2, capacity, false, true, false);
    cueBus.setSize(2, capacity, false, true, false);
    deckScratch.setSize(2, capacity, false, true, false);
    for (auto& slot : decks)
        slot.deck.prepare(sampleRate, capacity);
    clock.prepare(sampleRate);
    cueOutput.setWriter(sampleRate, capacity);
    for (auto& strip : strips)
        strip.prepare(sampleRate, capacity);
    for (std::size_t i = 0; i < mixer.size(); ++i)
    {
        auto& m = mixer[i];
        if (!mixerInitialised) // default assignment: deck A left, deck B right, others THRU
            m.assign = i == 0 ? CrossfaderAssign::A : i == 1 ? CrossfaderAssign::B : CrossfaderAssign::Thru;
        m.crossfaderGain.prepare(sampleRate, ChannelStrip::kSmoothingSeconds);
        m.meter.prepare(sampleRate);
        m.cueSend.prepare(sampleRate, ChannelStrip::kSmoothingSeconds);
    }
    for (auto* r : {&cueMixCue, &cueMixMaster, &headphoneGain})
        r->prepare(sampleRate, ChannelStrip::kSmoothingSeconds);
    cueLimiter.prepare(sampleRate, capacity);
    cueMeter.prepare(sampleRate);
    mixerInitialised = true;
    masterGain.prepare(sampleRate, ChannelStrip::kSmoothingSeconds);
    limiter.prepare(sampleRate, capacity);
    masterMeter.prepare(sampleRate);
    tone.gain.reset(sampleRate, kGainRampSeconds);
    reset();
}

void AudioEngine::reset() noexcept
{
    tone.phase = 0.0;
    tone.phaseIncrement = 2.0 * std::numbers::pi * tone.frequencyHz / sampleRate;
    tone.gain.setCurrentAndTargetValue(tone.on ? tone.gain.getTargetValue() : 0.0f);
    masterBus.clear();
    cueBus.clear();
    for (auto& m : mixer)
    {
        m.crossfaderGain.snap(crossfaderGain(m.assign, crossfaderPosition, crossfaderCurve));
        m.meter.reset();
        m.cueSend.snap(m.cue ? 1.0 : 0.0);
    }
    updateCueMix();
    cueMixCue.snap(cueMixCue.targetValue());
    cueMixMaster.snap(cueMixMaster.targetValue());
    headphoneGain.snap(headphoneGain.targetValue());
    cueLimiter.reset();
    cueMeter.reset();
    masterGain.snap(masterGainDb <= -100.0 ? 0.0 : std::pow(10.0, masterGainDb / 20.0));
    limiter.reset();
    masterMeter.reset();

    const auto processed = current.commandsProcessed;
    current = EngineState{};
    current.sampleRate = sampleRate;
    current.commandsProcessed = processed;
    current.outputLayout = layout;
    current.testToneOn = tone.on;
    current.testToneHz = tone.frequencyHz;
    current.testToneBus = tone.bus;
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    prepare(device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
}

void AudioEngine::audioDeviceStopped() {}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* /*inputs*/, int /*numInputs*/,
                                                   float* const* outputs, int numOutputs, int numSamples,
                                                   const juce::AudioIODeviceCallbackContext& /*context*/)
{
    process(outputs, numOutputs, numSamples);
}

void AudioEngine::process(float* const* outputs, int numChannels, int numSamples) noexcept
{
    util::ScopedRealtimeContext realtime;      // Debug: any allocation from here on is reported
    const juce::ScopedNoDenormals noDenormals; // decaying filter state must not hit the slow denormal path
    const auto startTicks = juce::Time::getHighResolutionTicks();

    retryPendingReleases();
    drainCommands();

    if (capacity == 0) // not prepared: stay silent
    {
        for (int ch = 0; ch < numChannels; ++ch)
            if (outputs[ch] != nullptr)
                juce::FloatVectorOperations::clear(outputs[ch], numSamples);
    }
    else
    {
        for (int offset = 0; offset < numSamples; offset += capacity)
            renderChunk(outputs, numChannels, offset, std::min(capacity, numSamples - offset));
    }

    const auto elapsed = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - startTicks);
    publish(numChannels, numSamples, elapsed);
}

void AudioEngine::renderChunk(float* const* outputs, int numChannels, int offset, int numSamples) noexcept
{
    masterBus.clear(0, numSamples);
    cueBus.clear(0, numSamples);

    advanceClock(numSamples); // before the decks render: the clock reads their playheads at the block start
    renderDecks(numSamples);
    renderMaster(numSamples);   // master gain
    mixHeadphones(numSamples);  // cue bus + master -> headphone level
    renderTestTone(numSamples); // straight onto its bus (not into the headphone mix)
    finishBuses(numSamples);    // limiters and meters
    swapPendingTracks();        // decks that finished fading out can take their new track now
    if (layout == OutputLayout::SeparateCueDevice) // P2-25: the headphones are on another device
        cueOutput.write(cueBus.getReadPointer(0), cueBus.getReadPointer(1), numSamples);

    // Point at this chunk of each device output channel.
    float* chunk[kMaxOutputChannels] = {};
    const int channels = std::min(numChannels, kMaxOutputChannels);
    for (int ch = 0; ch < channels; ++ch)
        chunk[ch] = outputs[ch] != nullptr ? outputs[ch] + offset : nullptr;
    for (int ch = channels; ch < numChannels; ++ch) // more outputs than we track: keep them silent
        if (outputs[ch] != nullptr)
            juce::FloatVectorOperations::clear(outputs[ch] + offset, numSamples);

    routeOutputs(layout, masterBus.getReadPointer(0), masterBus.getReadPointer(1), cueBus.getReadPointer(0),
                 cueBus.getReadPointer(1), chunk, channels, numSamples);
}

void AudioEngine::advanceClock(int numSamples) noexcept
{
    std::array<ClockDeckInput, kMaxDecks> inputs{};
    for (std::size_t i = 0; i < decks.size(); ++i)
    {
        const auto& slot = decks[i];
        auto& in = inputs[i];
        // A deck leads only while its timeline moves normally: slipping, its shadow is the timeline (P3-04);
        // reversed or braking without slip it doesn't lead (the clock runs on at its tempo, P3-05).
        const auto& d = slot.deck;
        in.playing = d.track() != nullptr && d.isPlaying() &&
                     (d.isSlipping() || (!d.isReverse() && !d.isStopping()));
        in.hasGrid = !slot.grid.empty();
        in.synced = slot.synced;
        if (in.hasGrid)
        {
            const double position = d.slipPosition();
            in.beat = slot.grid.beatAt(position);
            in.bpm = slot.grid.bpmAt(position) * slot.deck.rate();
        }
    }
    clock.beginBlock(numSamples, inputs);
    followMaster();
}

namespace
{
// The deck beats per master beat (1/2, 1 or 2) that make the rate closest to 1: a 64 BPM track syncs to a
// 128 BPM master at its own tempo, every second master beat.
double syncMultiple(double masterBpm, double gridBpm) noexcept
{
    double best = 1.0;
    for (const double q : {0.5, 1.0, 2.0})
        if (std::abs(std::log(masterBpm * q / gridBpm)) < std::abs(std::log(masterBpm * best / gridBpm)))
            best = q;
    return best;
}

double wrapHalf(double x) noexcept
{
    return x - std::floor(x + 0.5); // [-0.5, 0.5)
}

// When sync starts (pressed, or play on a synced deck) and the deck is further off than this, it jumps into phase
// once (with its 2 ms fade). Smaller errors - and the drift while it plays - the rate nudge closes: at most 0.5 %
// faster or slower, so 2 ms take 0.4 s, under a beat at any usual tempo.
constexpr double kSnapSeconds = 0.002;
constexpr double kCatchUpSeconds = 0.05; // the nudge aims to close the error in this long (then at most 0.5 %)
} // namespace

void AudioEngine::followMaster() noexcept
{
    const double masterBpm = clock.bpm();
    for (std::size_t i = 0; i < decks.size(); ++i)
    {
        auto& slot = decks[i];
        auto& deck = slot.deck;
        slot.phaseErrorMs = 0.0;
        if (!slot.synced || slot.grid.empty() || deck.track() == nullptr || static_cast<int>(i) == clock.sourceDeck())
        {
            deck.setRateCorrection(1.0);
            continue;
        }
        const double position = deck.position();
        const double gridBpm = slot.grid.bpmAt(position);
        if (!(gridBpm > 0.0) || !(masterBpm > 0.0))
            continue;
        // Tempo: the master's (or half / double), as heard.
        const double q = syncMultiple(masterBpm, gridBpm);
        deck.setSyncTempo((masterBpm * q / gridBpm - 1.0) * 100.0);
        if (!deck.isPlaying() || deck.isReverse() || deck.isStopping() || deck.isSlipping())
        {
            deck.setRateCorrection(1.0);
            continue;
        }
        // Phase: where its beat should be (the master's, in its own beats), + = behind.
        double error = wrapHalf(clock.beatAtBlockStart() * q - slot.grid.beatAt(position));
        if (slot.snapPending)
        {
            slot.snapPending = false;
            if (std::abs(error) * 60.0 / (masterBpm * q) > kSnapSeconds) // one jump, then the nudge keeps it
            {
                deck.seek(slot.grid.sampleAt(slot.grid.beatAt(position) + error));
                error = 0.0;
            }
        }
        const double errorSeconds = error * 60.0 / (masterBpm * q);
        slot.phaseErrorMs = 1000.0 * errorSeconds;
        deck.setRateCorrection(1.0 + std::clamp(errorSeconds / kCatchUpSeconds, -Deck::kMaxRateCorrection,
                                                Deck::kMaxRateCorrection));
    }
}

bool AudioEngine::tempoToMaster(DeckId deck, double percent) noexcept
{
    // Sync lock: a synced deck that follows can't set its own tempo; its fader sets the master's (so every
    // synced deck moves with it).
    auto& slot = decks[deck];
    const int lead = clock.sourceDeck();
    if (!slot.synced || slot.grid.empty() || lead == static_cast<int>(deck))
        return false;
    const double gridBpm = slot.grid.bpmAt(slot.deck.position());
    if (!(gridBpm > 0.0))
        return false;
    const double q = syncMultiple(clock.bpm(), gridBpm);
    const double wanted = gridBpm * (1.0 + percent / 100.0) / q; // the master tempo that gives this deck that tempo
    if (lead >= 0)
    {
        auto& master = decks[static_cast<std::size_t>(lead)];
        const double masterGrid = master.grid.bpmAt(master.deck.position());
        if (masterGrid > 0.0)
            master.deck.setSyncTempo((wanted / masterGrid - 1.0) * 100.0);
    }
    else
    {
        clock.setInternalBpm(wanted);
    }
    return true;
}

void AudioEngine::apply(const SetSync& command) noexcept
{
    if (command.deck >= kMaxDecks)
        return;
    auto& slot = decks[command.deck];
    slot.synced = command.on;
    slot.snapPending = command.on;
    if (!command.on)
        slot.deck.setRateCorrection(1.0);
}

void AudioEngine::apply(const SetClockSource& command) noexcept
{
    clock.setMode(command.mode, command.deck);
}

void AudioEngine::apply(const SetInternalTempo& command) noexcept
{
    clock.setInternalBpm(command.bpm);
}

void AudioEngine::drainCommands() noexcept
{
    EngineCommand command;
    for (int i = 0; i < kMaxCommandsPerBlock && commands.tryPop(command); ++i)
    {
        std::visit([this](const auto& c) { apply(c); }, command);
        ++current.commandsProcessed;
    }
}

void AudioEngine::apply(const SetTestTone& command) noexcept
{
    const float hz = std::clamp(command.frequencyHz, 20.0f, static_cast<float>(sampleRate * 0.45));
    tone.frequencyHz = hz;
    tone.phaseIncrement = 2.0 * std::numbers::pi * hz / sampleRate;

    const bool busChanged = command.bus != tone.bus;
    if (command.on && (!tone.on || busChanged))
    {
        tone.phase = 0.0; // start at a zero crossing; the gain ramp handles the rest
        if (busChanged)
            tone.gain.setCurrentAndTargetValue(0.0f); // fade in on the new bus instead of jumping
    }
    tone.on = command.on;
    tone.bus = command.bus;

    const double angle = (std::clamp(command.pan, -1.0f, 1.0f) + 1.0) * std::numbers::pi / 4.0;
    tone.gainLeft = static_cast<float>(std::cos(angle));
    tone.gainRight = static_cast<float>(std::sin(angle));
    tone.gain.setTargetValue(command.on ? juce::Decibels::decibelsToGain(command.gainDb) : 0.0f);

    current.testToneOn = tone.on;
    current.testToneHz = hz;
    current.testToneBus = tone.bus;
}

void AudioEngine::apply(const SetOutputLayout& command) noexcept
{
    layout = command.layout;
    current.outputLayout = layout;
}

void AudioEngine::apply(const LoadTrack& command) noexcept
{
    if (command.deck >= kMaxDecks)
    {
        releaseBuffer(command.bufferId); // nowhere to put it: give it straight back
        return;
    }
    requestTrack(command.deck, command.buffer, command.bufferId);
}

void AudioEngine::apply(const UnloadTrack& command) noexcept
{
    if (command.deck < kMaxDecks)
        requestTrack(command.deck, nullptr, 0);
}

void AudioEngine::apply(const SetBeatgrid& command) noexcept
{
    if (command.deck < kMaxDecks)
        decks[command.deck].grid = command.grid;
}

void AudioEngine::apply(const SetMetronome& command) noexcept
{
    if (command.deck >= kMaxDecks)
        return;
    auto& slot = decks[command.deck];
    slot.metronome = command.on;
    slot.metronomeGain = juce::Decibels::decibelsToGain(command.levelDb);
}

void AudioEngine::renderMetronome(std::size_t i, double before, double after, int numSamples) noexcept
{
    auto& slot = decks[i];
    // A new click on each beat crossed (forward play, no jump): at the sample where the playhead passed it.
    if (slot.metronome && !slot.grid.empty() && after > before && after - before < 8.0 * numSamples)
    {
        const auto& g = slot.grid;
        for (auto k = static_cast<long long>(std::floor(g.beatAt(before))) + 1;; ++k)
        {
            const double at = g.sampleAt(static_cast<double>(k));
            if (at > after)
                break;
            if (at <= before)
                continue;
            const bool downbeat = g.barBeatOf(k) == 1;
            slot.clickDelay = static_cast<int>(std::floor((at - before) / (after - before) * numSamples));
            slot.clickPosition = 0;
            slot.clickLength = static_cast<int>(0.015 * sampleRate);
            slot.clickIncrement = 2.0 * std::numbers::pi * (downbeat ? 2000.0 : 1000.0) / sampleRate;
            slot.clickLevel = slot.metronomeGain * (downbeat ? 1.0f : 0.7f);
            slot.clickActive = true;
        }
    }
    if (!slot.clickActive)
        return;
    float* cl = cueBus.getWritePointer(0);
    float* cr = cueBus.getWritePointer(1);
    const int start = std::max(0, slot.clickDelay);
    const double decay = 1.0 / (0.003 * sampleRate);
    for (int s = start; s < numSamples && slot.clickPosition < slot.clickLength; ++s, ++slot.clickPosition)
    {
        const double p = slot.clickPosition;
        const auto v = static_cast<float>(slot.clickLevel * std::exp(-p * decay) * std::sin(p * slot.clickIncrement));
        cl[s] += v;
        cr[s] += v;
    }
    slot.clickDelay = 0; // the rest of it starts at the top of the next chunk
    if (slot.clickPosition >= slot.clickLength)
        slot.clickActive = false;
}

void AudioEngine::requestTrack(DeckId deckId, const TrackBuffer* buffer, BufferId id) noexcept
{
    auto& slot = decks[deckId];
    slot.grid = {};
    slot.queuedCount = 0;
    slot.hotCues = {}; // the app sends the new track's
    slot.hotCuePreview = -1;
    slot.slipHoldPad = -1;
    slot.slicerHeld = 0;
    slot.slicerRolling = -1;
    slot.slicerStartBeat = -1e9; // placed again on the new track
    slot.loopInPending = false;
    slot.haveLastLoop = false;
    slot.clickActive = false;
    if (slot.hasPending && slot.pendingId != id)
        releaseBuffer(slot.pendingId); // superseded before it was ever used
    slot.hasPending = true;
    slot.pendingBuffer = buffer;
    slot.pendingId = id;
    slot.deck.pause(); // fade out first if it was sounding
    swapPendingTracks();
}

void AudioEngine::swapPendingTracks() noexcept
{
    for (auto& slot : decks)
    {
        if (!slot.hasPending || !slot.deck.isSilent())
            continue;
        if (slot.id != 0 && slot.id != slot.pendingId)
            releaseBuffer(slot.id); // nothing reads it any more
        slot.deck.setTrack(slot.pendingBuffer);
        slot.id = slot.pendingId;
        slot.hasPending = false;
        slot.pendingBuffer = nullptr;
        slot.pendingId = 0;
    }
}

void AudioEngine::apply(const Play& command) noexcept
{
    if (command.deck >= kMaxDecks || decks[command.deck].hasPending)
        return;
    auto& slot = decks[command.deck];
    if (slot.hotCuePreview >= 0) // playing from a held hot cue: Play keeps it playing (P3-01)
    {
        slot.hotCuePreview = -1;
        return;
    }
    // Quantized, with a deck leading the clock: start on the clock's next grid point (in time with it).
    const bool quantized = (quantizeOn || command.quantized) && clock.sourceDeck() >= 0 &&
                           clock.sourceDeck() != static_cast<int>(command.deck) && !slot.deck.isPlaying();
    if (quantized)
    {
        const double next = std::ceil(clock.beatAtBlockStart() / quantizeBeats - 1e-9) * quantizeBeats;
        queue(slot, {DeckSlot::QueuedAction::Kind::Play, next, 0.0});
        return;
    }
    startPlaying(slot);
}

void AudioEngine::startPlaying(DeckSlot& slot) noexcept
{
    slot.deck.play();
    slot.snapPending = slot.synced; // into phase as it starts (P2-19)
}

void AudioEngine::queue(DeckSlot& slot, DeckSlot::QueuedAction action) noexcept
{
    if (slot.queuedCount < DeckSlot::kMaxQueued) // full: the newest is dropped (16 waiting is plenty)
        slot.queued[static_cast<std::size_t>(slot.queuedCount++)] = action;
}

void AudioEngine::apply(const SetQuantize& command) noexcept
{
    quantizeOn = command.on;
    // 1/8, 1/4, 1/2 or 1 beat (the nearest).
    double best = 1.0;
    for (const double b : {0.125, 0.25, 0.5, 1.0})
        if (std::abs(b - command.beats) < std::abs(best - command.beats))
            best = b;
    quantizeBeats = best;
}

void AudioEngine::apply(const BeatJump& command) noexcept
{
    if (command.deck >= kMaxDecks || decks[command.deck].hasPending)
        return;
    auto& slot = decks[command.deck];
    const auto& g = slot.grid;
    if (g.empty() || slot.deck.track() == nullptr || command.beats == 0.0)
        return;
    const double length = static_cast<double>(slot.deck.length());
    const auto clampToTrack = [&](double sample) { return std::clamp(sample, 0.0, length); };
    // Looping: the loop moves by the beats, and the playhead with it (P3-03).
    if (slot.deck.loopActive())
    {
        const double a = g.sampleAt(g.beatAt(slot.deck.loopStart()) + command.beats);
        const double b = g.sampleAt(g.beatAt(slot.deck.loopEnd()) + command.beats);
        if (a < 0.0 || b > length)
            return; // it would leave the track
        const double into = slot.deck.position() - slot.deck.loopStart();
        slot.deck.setLoop(a, b);
        slot.haveLastLoop = true;
        slot.lastLoopStart = a;
        slot.lastLoopEnd = b;
        slot.deck.seek(a + std::max(0.0, into));
        return;
    }
    // A jump already waiting for its grid point: this one adds to it.
    if (slot.queuedCount > 0)
    {
        auto& last = slot.queued[static_cast<std::size_t>(slot.queuedCount - 1)];
        if (last.kind == DeckSlot::QueuedAction::Kind::Jump)
        {
            last.to = clampToTrack(g.sampleAt(g.beatAt(last.to) + command.beats));
            return;
        }
    }
    const double beat = g.beatAt(slot.deck.position());
    if (quantizeOn && slot.deck.isPlaying() && slot.deck.rate() > 0.0)
    {
        const double next = (std::floor(beat / quantizeBeats + 1e-9) + 1.0) * quantizeBeats;
        queue(slot, {DeckSlot::QueuedAction::Kind::Jump, g.sampleAt(next),
                     clampToTrack(g.sampleAt(next + command.beats))});
        return;
    }
    slot.deck.seek(clampToTrack(g.sampleAt(beat + command.beats)));
}

void AudioEngine::apply(const Loop& command) noexcept
{
    if (command.deck >= kMaxDecks || decks[command.deck].hasPending)
        return;
    auto& slot = decks[command.deck];
    auto& deck = slot.deck;
    if (deck.track() == nullptr)
        return;
    const auto& g = slot.grid;
    const auto clampBeats = [](double b) { return std::clamp(b, 1.0 / 64.0, 512.0); };
    // With quantize on: the grid point at or before a position (in points), the nearest one (out points).
    const auto floorToGrid = [&](double at)
    {
        if (!quantizeOn || g.empty())
            return at;
        return g.sampleAt(std::floor(g.beatAt(at) / quantizeBeats + 1e-6) * quantizeBeats);
    };
    const auto roundToGrid = [&](double at)
    {
        if (!quantizeOn || g.empty())
            return at;
        return g.sampleAt(std::round(g.beatAt(at) / quantizeBeats) * quantizeBeats);
    };
    const auto start = [&](double a, double b)
    {
        deck.setLoop(a, b);
        if (deck.loopActive())
        {
            slot.haveLastLoop = true;
            slot.lastLoopStart = deck.loopStart();
            slot.lastLoopEnd = deck.loopEnd();
        }
    };
    // A loop of `beats` from a grid position (beats count on the grid, so a tempo change in it is followed).
    const auto beatsFrom = [&](double at, double beats) { return g.sampleAt(g.beatAt(at) + beats); };

    switch (command.op)
    {
        case LoopOp::Auto:
        {
            if (deck.loopActive())
            {
                deck.exitLoop();
                return;
            }
            if (g.empty())
                return;
            const double beats = clampBeats(command.beats > 0.0 ? command.beats : slot.autoLoopBeats);
            slot.autoLoopBeats = beats;
            double from = deck.position();
            if (quantizeOn)
                from = g.sampleAt(std::floor(g.beatAt(from) / std::min(quantizeBeats, beats) + 1e-6) *
                                  std::min(quantizeBeats, beats));
            start(from, beatsFrom(from, beats));
            return;
        }
        case LoopOp::In:
            slot.loopInPending = true;
            slot.loopInPoint = floorToGrid(deck.position());
            return;
        case LoopOp::Out:
        {
            if (!slot.loopInPending)
                return;
            const double out = roundToGrid(deck.position());
            if (out > slot.loopInPoint + 1.0)
            {
                start(slot.loopInPoint, out);
                slot.loopInPending = false;
            }
            return;
        }
        case LoopOp::Halve:
        case LoopOp::Double:
        {
            const double factor = command.op == LoopOp::Halve ? 0.5 : 2.0;
            if (!deck.loopActive())
            {
                slot.autoLoopBeats = clampBeats(slot.autoLoopBeats * factor);
                return;
            }
            const double a = deck.loopStart();
            double b = a + (deck.loopEnd() - a) * factor;
            if (!g.empty())
            {
                const double beats = clampBeats((g.beatAt(deck.loopEnd()) - g.beatAt(a)) * factor);
                b = beatsFrom(a, beats);
            }
            start(a, b); // past the new end: the deck wraps at once
            return;
        }
        case LoopOp::Reloop:
            if (!slot.haveLastLoop)
                return;
            start(slot.lastLoopStart, slot.lastLoopEnd);
            deck.seek(slot.lastLoopStart);
            return;
        case LoopOp::Exit:
            deck.exitLoop();
            return;
        case LoopOp::Beat:
        {
            if (g.empty())
                return;
            const double beats = clampBeats(command.beats > 0.0 ? command.beats : slot.autoLoopBeats);
            if (deck.loopActive())
            {
                const double a = deck.loopStart();
                if (std::abs((g.beatAt(deck.loopEnd()) - g.beatAt(a)) - beats) < 1e-6)
                    deck.exitLoop(); // the same pad again
                else
                    start(a, beatsFrom(a, beats)); // another size: from the same start
                return;
            }
            double from = deck.position();
            if (quantizeOn)
                from = g.sampleAt(std::floor(g.beatAt(from) / std::min(quantizeBeats, beats) + 1e-6) *
                                  std::min(quantizeBeats, beats));
            start(from, beatsFrom(from, beats));
            return;
        }
        case LoopOp::Set:
            start(command.start, command.end);
            if (deck.loopActive())
                deck.seek(command.start);
            return;
    }
}

void AudioEngine::apply(const SetCuePoint& command) noexcept
{
    if (command.deck < kMaxDecks && decks[command.deck].deck.track() != nullptr)
        decks[command.deck].deck.setCuePoint(command.samplePos);
}

void AudioEngine::apply(const SetHotCues& command) noexcept
{
    if (command.deck < kMaxDecks)
        decks[command.deck].hotCues = command.cues;
}

void AudioEngine::apply(const HotCue& command) noexcept
{
    if (command.deck >= kMaxDecks || command.slot >= kHotCues || decks[command.deck].hasPending)
        return;
    auto& slot = decks[command.deck];
    auto& deck = slot.deck;
    auto& cue = slot.hotCues[command.slot];
    if (deck.track() == nullptr)
        return;
    switch (command.op)
    {
        case HotCueOp::Delete:
            cue = {};
            if (slot.hotCuePreview == command.slot)
                slot.hotCuePreview = -1;
            return;

        case HotCueOp::Release:
            // Held from pause and not taken over by Play: back to the cue, paused.
            if (slot.hotCuePreview == command.slot)
            {
                slot.hotCuePreview = -1;
                deck.pause();
                deck.seek(cue.start);
            }
            if (slot.slipHoldPad == command.slot) // slip: back to where the track would be (P3-04)
            {
                slot.slipHoldPad = -1;
                int kept = 0; // a quantized jump to it that hasn't happened yet is called off
                for (int k = 0; k < slot.queuedCount; ++k)
                {
                    const auto& a = slot.queued[static_cast<std::size_t>(k)];
                    if (a.kind == DeckSlot::QueuedAction::Kind::Play || a.loopLength > 0.0)
                        slot.queued[static_cast<std::size_t>(kept++)] = a;
                }
                slot.queuedCount = kept;
                deck.endSlipHold();
            }
            return;

        case HotCueOp::Press:
            break;
    }
    if (!cue.set && deck.loopActive())
    {
        // A loop plays: the pad stores it (a hot loop).
        cue = {true, deck.loopStart(), deck.loopEnd() - deck.loopStart()};
        return;
    }
    const bool quantizedJump = deck.transport() == Deck::Transport::Playing && quantizeOn && !slot.grid.empty() &&
                               deck.rate() > 0.0;
    if (cue.set && cue.length > 0.0)
    {
        // A hot loop: loop it from its start (quantized while playing: once the jump is there).
        slot.haveLastLoop = true;
        slot.lastLoopStart = cue.start;
        slot.lastLoopEnd = cue.start + cue.length;
        if (!quantizedJump)
            deck.setLoop(cue.start, cue.start + cue.length);
    }
    if (!cue.set)
    {
        // Set it here (quantize: on the nearest grid point).
        double at = deck.position();
        if (quantizeOn && !slot.grid.empty())
            at = slot.grid.sampleAt(std::round(slot.grid.beatAt(at) / quantizeBeats) * quantizeBeats);
        cue = {true, std::clamp(at, 0.0, static_cast<double>(deck.length())), 0.0};
        return;
    }
    if (deck.transport() == Deck::Transport::Playing)
    {
        if (deck.slipOn() && cue.length <= 0.0) // slip: the pad plays from the cue while held (P3-04)
        {
            deck.beginSlipHold();
            slot.slipHoldPad = command.slot;
        }
        if (quantizeOn && !slot.grid.empty() && deck.rate() > 0.0)
        {
            const double next =
                (std::floor(slot.grid.beatAt(deck.position()) / quantizeBeats + 1e-9) + 1.0) * quantizeBeats;
            queue(slot, {DeckSlot::QueuedAction::Kind::Jump, slot.grid.sampleAt(next), cue.start, cue.length});
        }
        else
        {
            deck.seek(cue.start);
        }
        return;
    }
    // Paused (or the CUE button held): play from the hot cue while the pad is held.
    if (deck.transport() == Deck::Transport::CuePreview)
        deck.cueRelease();
    deck.seek(cue.start);
    startPlaying(slot);
    slot.hotCuePreview = command.slot;
}

void AudioEngine::apply(const SetSlip& command) noexcept
{
    if (command.deck >= kMaxDecks)
        return;
    decks[command.deck].deck.setSlip(command.on);
    if (!command.on)
        decks[command.deck].slipHoldPad = -1;
}

void AudioEngine::apply(const SetReverse& command) noexcept
{
    if (command.deck < kMaxDecks && !decks[command.deck].hasPending)
        decks[command.deck].deck.setReverse(command.on);
}

void AudioEngine::apply(const Stop& command) noexcept
{
    if (command.deck >= kMaxDecks || decks[command.deck].hasPending)
        return;
    auto& slot = decks[command.deck];
    if (command.kind == StopKind::Instant || slot.deck.transport() != Deck::Transport::Playing ||
        slot.deck.isScratching())
    {
        if (slot.deck.isPlaying())
            apply(Pause{command.deck});
        return;
    }
    if (slot.deck.isStopping())
        return;
    slot.hotCuePreview = -1;
    if (command.kind == StopKind::Brake)
        slot.deck.brake(brakeSeconds);
    else
        slot.deck.spinBack(spinBackSeconds);
}

void AudioEngine::apply(const SetStopStyle& command) noexcept
{
    pauseStyle = command.pause;
    brakeSeconds = std::clamp(static_cast<double>(command.brakeSeconds), 0.05, 10.0);
    spinBackSeconds = std::clamp(static_cast<double>(command.spinBackSeconds), 0.1, 10.0);
}

void AudioEngine::updateSlicer(DeckSlot& slot) noexcept
{
    if (!slot.slicerOn || slot.grid.empty() || slot.deck.track() == nullptr)
        return;
    const double n = slot.slicerBeats;
    const double beat = slot.grid.beatAt(slot.deck.slipPosition()); // the track's timeline
    if (beat < slot.slicerStartBeat || beat >= slot.slicerStartBeat + 2.0 * n) // placed anew (on, a seek)
        slot.slicerStartBeat = std::floor(beat / 4.0 + 1e-9) * 4.0;              // the bar at or before
    else if (beat >= slot.slicerStartBeat + n)
        slot.slicerStartBeat += n; // domain shift: the next N beats
}

void AudioEngine::dropQueuedJumps(DeckSlot& slot) noexcept
{
    int kept = 0;
    for (int k = 0; k < slot.queuedCount; ++k)
    {
        const auto& a = slot.queued[static_cast<std::size_t>(k)];
        if (a.kind == DeckSlot::QueuedAction::Kind::Play)
            slot.queued[static_cast<std::size_t>(kept++)] = a;
    }
    slot.queuedCount = kept;
}

void AudioEngine::rollSlice(DeckSlot& slot, int pad) noexcept
{
    const auto& g = slot.grid;
    auto& deck = slot.deck;
    const double sliceBeats = slot.slicerBeats / 8.0;
    const double a = g.sampleAt(slot.slicerStartBeat + pad * sliceBeats);
    const double b = g.sampleAt(slot.slicerStartBeat + (pad + 1) * sliceBeats);
    slot.slicerRolling = pad;
    dropQueuedJumps(slot);
    // Always on the grid: from the next slice boundary (where the playhead is, also inside a rolling slice).
    const double next = (std::floor(g.beatAt(deck.position()) / sliceBeats + 1e-9) + 1.0) * sliceBeats;
    queue(slot, {DeckSlot::QueuedAction::Kind::Jump, g.sampleAt(next), a, b - a});
}

void AudioEngine::apply(const SetSlicer& command) noexcept
{
    if (command.deck >= kMaxDecks)
        return;
    auto& slot = decks[command.deck];
    const double beats = command.beats >= 24.0 ? 32.0 : command.beats >= 12.0 ? 16.0 : 8.0;
    if (command.on && (!slot.slicerOn || beats != slot.slicerBeats))
        slot.slicerStartBeat = -1e9; // placed on the next block
    slot.slicerBeats = beats;
    if (!command.on && slot.slicerOn && slot.slicerHeld != 0) // a slice rolling: let it go
    {
        slot.slicerHeld = 0;
        slot.slicerRolling = -1;
        dropQueuedJumps(slot);
        slot.deck.exitLoop();
        slot.deck.endSlicerHold();
    }
    slot.slicerOn = command.on;
    updateSlicer(slot);
}

void AudioEngine::apply(const SlicerPad& command) noexcept
{
    if (command.deck >= kMaxDecks || command.pad >= 8 || decks[command.deck].hasPending)
        return;
    auto& slot = decks[command.deck];
    auto& deck = slot.deck;
    const auto bit = static_cast<std::uint8_t>(1u << command.pad);
    if (command.release)
    {
        if ((slot.slicerHeld & bit) == 0)
            return;
        slot.slicerHeld = static_cast<std::uint8_t>(slot.slicerHeld & ~bit);
        if (slot.slicerHeld != 0)
        {
            if (slot.slicerRolling == command.pad) // another pad is still down: roll that one
                for (int p = 7; p >= 0; --p)
                    if ((slot.slicerHeld & (1u << p)) != 0)
                    {
                        rollSlice(slot, p);
                        break;
                    }
            return;
        }
        // The last pad up: back to where the track would be.
        slot.slicerRolling = -1;
        dropQueuedJumps(slot);
        deck.exitLoop();
        deck.endSlicerHold();
        return;
    }
    if (!slot.slicerOn || slot.grid.empty() || deck.transport() != Deck::Transport::Playing || deck.isReverse() ||
        deck.rate() <= 0.0)
        return;
    if (slot.slicerHeld == 0)
        deck.beginSlicerHold();
    slot.slicerHeld = static_cast<std::uint8_t>(slot.slicerHeld | bit);
    rollSlice(slot, command.pad);
}

// ---- FX (P3-08) ----

bool AudioEngine::installEffect(int unit, int slot, std::unique_ptr<dsp::fx::Effect> effect)
{
    if (unit < 0 || unit >= kFxUnits || slot < 0 || slot >= kFxSlots)
        return false;
    dsp::fx::Effect* raw = effect.get();
    if (effect != nullptr)
    {
        effect->prepare(sampleRate, preparedBlockSize);
        ownedEffects.push_back(std::move(effect));
    }
    if (send(FxSetEffect{static_cast<std::uint8_t>(unit), static_cast<std::uint8_t>(slot), raw}))
        return true;
    if (raw != nullptr)
        ownedEffects.pop_back();
    return false;
}

int AudioEngine::collectEffects()
{
    int collected = 0;
    dsp::fx::Effect* e = nullptr;
    while (releasedEffects.tryPop(e))
    {
        const auto it = std::find_if(ownedEffects.begin(), ownedEffects.end(),
                                     [e](const auto& owned) { return owned.get() == e; });
        if (it != ownedEffects.end())
        {
            ownedEffects.erase(it);
            ++collected;
        }
    }
    return collected;
}

void AudioEngine::retireEffect(dsp::fx::Effect* effect) noexcept
{
    if (effect == nullptr)
        return;
    if (releasedEffects.tryPush(effect))
        return;
    if (numPendingEffects < kMaxPendingEffects)
        pendingEffects[static_cast<std::size_t>(numPendingEffects++)] = effect;
    // (more than that in one go: it stays owned until the engine goes away)
}

void AudioEngine::apply(const FxSetEffect& command) noexcept
{
    if (command.unit >= kFxUnits || command.slot >= kFxSlots)
        return;
    auto& slot = fxUnits[command.unit].slot(command.slot);
    retireEffect(slot.effect());
    slot.setEffect(command.effect);
    if (command.effect != nullptr)
        command.effect->setBeats(fxBeats[command.unit][command.slot] > 0.0f ? fxBeats[command.unit][command.slot]
                                                                             : 1.0);
}

void AudioEngine::apply(const FxCommand& command) noexcept
{
    if (command.unit >= kFxUnits)
        return;
    auto& unit = fxUnits[command.unit];
    const int s = std::min<int>(command.slot, kFxSlots - 1);
    auto& slot = unit.slot(s);
    switch (command.op)
    {
        case FxOp::On:
            if (command.value == 0.0f || unit.slotUsable(s))
                slot.setOn(command.value != 0.0f);
            break;
        case FxOp::Wet:
            slot.setWet(command.value);
            break;
        case FxOp::Beats:
            fxBeats[command.unit][static_cast<std::size_t>(s)] = std::clamp(command.value, 1.0f / 16.0f, 16.0f);
            if (slot.effect() != nullptr)
                slot.effect()->setBeats(fxBeats[command.unit][static_cast<std::size_t>(s)]);
            break;
        case FxOp::Param:
            if (slot.effect() != nullptr)
                slot.effect()->setParam(command.index, std::clamp(command.value, 0.0f, 1.0f));
            break;
        case FxOp::Target:
        {
            const int t = static_cast<int>(command.value);
            if (t >= 0 && t < kFxTargetCount)
            {
                fxTargets[command.unit] = static_cast<FxTarget>(t);
                unit.reset(); // no tail carried from one place to another (switched-on slots start again)
            }
            break;
        }
        case FxOp::Mode:
            unit.setMode(command.value != 0.0f ? dsp::fx::FxUnit::Mode::Single : dsp::fx::FxUnit::Mode::Multi);
            break;
    }
}

void AudioEngine::processFx(FxTarget target, float* left, float* right, int numSamples) noexcept
{
    dsp::fx::BeatContext beat;
    beat.bpm = clock.bpm();
    beat.beatAtBlockStart = clock.beatAtBlockStart();
    beat.beatsPerSample = clock.beatsPerSample();
    for (int u = 0; u < kFxUnits; ++u)
        if (fxTargets[static_cast<std::size_t>(u)] == target)
            fxUnits[static_cast<std::size_t>(u)].process(left, right, numSamples, beat);
}

void AudioEngine::apply(const JumpBy& command) noexcept
{
    if (command.deck >= kMaxDecks || decks[command.deck].hasPending)
        return;
    auto& deck = decks[command.deck].deck;
    if (deck.track() != nullptr)
        deck.seek(deck.position() + command.seconds * sampleRate);
}

void AudioEngine::renderDeckWithActions(DeckSlot& slot, float* l, float* r, int numSamples) noexcept
{
    int done = 0;
    while (slot.queuedCount > 0)
    {
        auto& a = slot.queued[0];
        // Where in this chunk the first one falls due (actions run in order).
        int due = numSamples;
        double overshoot = 0.0;
        if (a.kind == DeckSlot::QueuedAction::Kind::Play)
        {
            const double offset = clock.sampleAt(a.at) - static_cast<double>(clock.blockStart());
            due = static_cast<int>(std::max(0.0, std::round(offset)));
        }
        else
        {
            const double rate = slot.deck.rate();
            if (!slot.deck.isPlaying() || rate <= 0.0 || slot.deck.isReverse() || slot.deck.isStopping() ||
                a.at < slot.deck.position())
            {
                due = done; // not moving forward, or its grid point is already behind (an earlier jump): now
            }
            else
            {
                const double steps = std::ceil((a.at - slot.deck.position()) / rate - 1e-9);
                due = done + static_cast<int>(std::max(0.0, std::min(steps, 1e9)));
                overshoot = slot.deck.position() + (due - done) * rate - a.at;
            }
        }
        if (due >= numSamples)
            break;
        if (due > done)
        {
            slot.deck.render(l + done, r + done, due - done);
            done = due;
        }
        const auto action = a;
        for (int k = 1; k < slot.queuedCount; ++k)
            slot.queued[static_cast<std::size_t>(k - 1)] = slot.queued[static_cast<std::size_t>(k)];
        --slot.queuedCount;
        if (action.kind == DeckSlot::QueuedAction::Kind::Play)
        {
            startPlaying(slot);
        }
        else
        {
            if (action.loopLength > 0.0) // a hot loop: looping from its start, now that the jump is there
                slot.deck.setLoop(action.to, action.to + action.loopLength); // first: slip's shadow starts here
            slot.deck.seek(action.to + std::max(0.0, overshoot));
        }
    }
    if (done < numSamples)
        slot.deck.render(l + done, r + done, numSamples - done);
}

void AudioEngine::apply(const Pause& command) noexcept
{
    if (command.deck >= kMaxDecks)
        return;
    auto& slot = decks[command.deck];
    slot.deck.pause();
    slot.hotCuePreview = -1;
    // A quantized play still waiting is called off.
    int kept = 0;
    for (int k = 0; k < slot.queuedCount; ++k)
        if (slot.queued[static_cast<std::size_t>(k)].kind != DeckSlot::QueuedAction::Kind::Play)
            slot.queued[static_cast<std::size_t>(kept++)] = slot.queued[static_cast<std::size_t>(k)];
    slot.queuedCount = kept;
}

void AudioEngine::apply(const TogglePlay& command) noexcept
{
    if (command.deck >= kMaxDecks || decks[command.deck].hasPending)
        return;
    const auto& slot = decks[command.deck];
    const bool waiting = std::any_of(slot.queued.begin(), slot.queued.begin() + slot.queuedCount,
                                     [](const auto& a) { return a.kind == DeckSlot::QueuedAction::Kind::Play; });
    if (slot.deck.isStopping())
        apply(Play{command.deck}); // during a brake / spin-back: back to playing (P3-05)
    else if (slot.deck.transport() == Deck::Transport::Playing && !waiting && pauseStyle != StopKind::Instant)
        apply(Stop{command.deck, pauseStyle});
    else if (slot.deck.transport() == Deck::Transport::Playing || waiting)
        apply(Pause{command.deck});
    else
        apply(Play{command.deck}); // from a held cue: keeps playing
}

void AudioEngine::apply(const CuePress& command) noexcept
{
    if (command.deck >= kMaxDecks || decks[command.deck].hasPending)
        return;
    auto& slot = decks[command.deck];
    // Quantized cue set: paused, the cue (and the playhead) snap to the nearest grid point first.
    if (quantizeOn && !slot.grid.empty() && slot.deck.transport() == Deck::Transport::Paused)
    {
        const double beat = std::round(slot.grid.beatAt(slot.deck.position()) / quantizeBeats) * quantizeBeats;
        slot.deck.seek(slot.grid.sampleAt(beat));
    }
    slot.deck.cuePress();
}

void AudioEngine::apply(const CueRelease& command) noexcept
{
    if (command.deck < kMaxDecks)
        decks[command.deck].deck.cueRelease();
}

void AudioEngine::apply(const Seek& command) noexcept
{
    if (command.deck >= kMaxDecks || decks[command.deck].hasPending)
        return;
    auto& slot = decks[command.deck];
    if ((quantizeOn || command.quantized) && !slot.grid.empty())
    {
        // Land on a grid point; while playing, jump as the playhead crosses its next one (the phase stays).
        const auto& g = slot.grid;
        const double to = g.sampleAt(std::round(g.beatAt(command.samplePos) / quantizeBeats) * quantizeBeats);
        if (slot.deck.isPlaying() && slot.deck.rate() > 0.0)
        {
            const double next =
                (std::floor(g.beatAt(slot.deck.position()) / quantizeBeats + 1e-9) + 1.0) * quantizeBeats;
            queue(slot, {DeckSlot::QueuedAction::Kind::Jump, g.sampleAt(next), to});
            return;
        }
        slot.deck.seek(to);
        return;
    }
    slot.deck.seek(command.samplePos);
}

void AudioEngine::apply(const SetTempo& command) noexcept
{
    if (command.deck < kMaxDecks && !tempoToMaster(command.deck, command.percent))
        decks[command.deck].deck.setTempoPercent(command.percent);
}

void AudioEngine::apply(const NudgeTempo& command) noexcept
{
    if (command.deck < kMaxDecks &&
        !tempoToMaster(command.deck, decks[command.deck].deck.tempoPercent() + command.deltaPercent))
        decks[command.deck].deck.nudgeTempo(command.deltaPercent);
}

void AudioEngine::apply(const SetTempoRange& command) noexcept
{
    if (command.deck < kMaxDecks)
        decks[command.deck].deck.setTempoRange(command.rangePercent);
}

void AudioEngine::apply(const ResetTempo& command) noexcept
{
    if (command.deck < kMaxDecks && !tempoToMaster(command.deck, 0.0))
        decks[command.deck].deck.resetTempo();
}

void AudioEngine::apply(const PitchBend& command) noexcept
{
    if (command.deck < kMaxDecks)
        decks[command.deck].deck.setPitchBend(command.percent);
}

void AudioEngine::apply(const SetInterpolationQuality& command) noexcept
{
    const auto q = command.quality == InterpolationQuality::High ? dsp::Resampler::Quality::Sinc16
                                                                 : dsp::Resampler::Quality::Hermite;
    for (auto& slot : decks)
        slot.deck.setInterpolation(q);
}

void AudioEngine::apply(const SetKeyLock& command) noexcept
{
    if (command.deck < kMaxDecks)
        decks[command.deck].deck.setKeyLock(command.on);
}

void AudioEngine::apply(const SetKeyShift& command) noexcept
{
    if (command.deck < kMaxDecks)
        decks[command.deck].deck.setKeyShift(command.semitones);
}

void AudioEngine::apply(const ScratchBegin& command) noexcept
{
    if (command.deck < kMaxDecks && !decks[command.deck].hasPending)
        decks[command.deck].deck.scratchBegin(command.timeSeconds);
}

void AudioEngine::apply(const ScratchMove& command) noexcept
{
    if (command.deck < kMaxDecks)
        decks[command.deck].deck.scratchMove(command.deltaSamples, command.timeSeconds);
}

void AudioEngine::apply(const ScratchEnd& command) noexcept
{
    if (command.deck < kMaxDecks)
        decks[command.deck].deck.scratchEnd();
}

void AudioEngine::apply(const Nudge& command) noexcept
{
    if (command.deck < kMaxDecks && !decks[command.deck].hasPending)
        decks[command.deck].deck.nudge(command.deltaSamples);
}

void AudioEngine::updateCrossfaderGains() noexcept
{
    for (auto& m : mixer)
        m.crossfaderGain.setTarget(crossfaderGain(m.assign, crossfaderPosition, crossfaderCurve));
}

void AudioEngine::renderMaster(int numSamples) noexcept
{
    // Master gain (the limiter runs in finishBuses, after the headphone mix has taken its copy).
    float* l = masterBus.getWritePointer(0);
    float* r = masterBus.getWritePointer(1);
    processFx(FxTarget::Master, l, r, numSamples); // FX units on the master (P3-08)
    if (masterGain.isRamping() || masterGain.value() != 1.0)
        for (int i = 0; i < numSamples; ++i)
        {
            const auto g = static_cast<float>(masterGain.next());
            l[i] *= g;
            r[i] *= g;
        }
}

void AudioEngine::updateCueMix() noexcept
{
    // Mix knob: cue at full level up to the centre, then fading out; master the mirror image. Centre = both.
    cueMixCue.setTarget(std::min(1.0, 2.0 * (1.0 - cueMix)));
    cueMixMaster.setTarget(std::min(1.0, 2.0 * cueMix));
    headphoneGain.setTarget(headphoneLevelDb <= -100.0 ? 0.0 : std::pow(10.0, headphoneLevelDb / 20.0));
}

void AudioEngine::mixHeadphones(int numSamples) noexcept
{
    // cueBus holds the pre-fader sum of channels with CUE on; add the master (after master gain, before its
    // limiter, so both go through one limiter each and stay in step).
    float* cl = cueBus.getWritePointer(0);
    float* cr = cueBus.getWritePointer(1);
    const float* ml = masterBus.getReadPointer(0);
    const float* mr = masterBus.getReadPointer(1);
    for (int i = 0; i < numSamples; ++i)
    {
        const auto gc = static_cast<float>(cueMixCue.next());
        const auto gm = static_cast<float>(cueMixMaster.next());
        const auto level = static_cast<float>(headphoneGain.next());
        cl[i] = (gc * cl[i] + gm * ml[i]) * level;
        cr[i] = (gc * cr[i] + gm * mr[i]) * level;
    }
}

void AudioEngine::finishBuses(int numSamples) noexcept
{
    float* l = masterBus.getWritePointer(0);
    float* r = masterBus.getWritePointer(1);
    limiter.process(l, r, numSamples);
    blockGainReductionDb = std::max(blockGainReductionDb, limiter.lastGainReductionDb());
    masterMeter.process(l, r, numSamples);

    float* cl = cueBus.getWritePointer(0);
    float* cr = cueBus.getWritePointer(1);
    cueLimiter.process(cl, cr, numSamples);
    cueMeter.process(cl, cr, numSamples);
}

void AudioEngine::apply(const SetParam& command) noexcept
{
    switch (command.param)
    {
        case ParamId::Crossfader:
            crossfaderPosition = std::clamp(static_cast<double>(command.value), 0.0, 1.0);
            updateCrossfaderGains();
            return;
        case ParamId::CrossfaderCurve:
            crossfaderCurve = command.value >= 1.5   ? CrossfaderCurve::Cut
                              : command.value >= 0.5 ? CrossfaderCurve::Linear
                                                     : CrossfaderCurve::Smooth;
            updateCrossfaderGains();
            return;
        case ParamId::CueMix:
            cueMix = std::clamp(static_cast<double>(command.value), 0.0, 1.0);
            updateCueMix();
            return;
        case ParamId::HeadphoneLevel:
            headphoneLevelDb = command.value <= -100.0f ? -100.0 : std::min(6.0, static_cast<double>(command.value));
            updateCueMix();
            return;
        case ParamId::MasterGain:
            masterGainDb = command.value <= -100.0f ? -100.0 : std::min(6.0, static_cast<double>(command.value));
            masterGain.setTarget(masterGainDb <= -100.0 ? 0.0 : std::pow(10.0, masterGainDb / 20.0));
            return;
        default:
            break;
    }
    if (!isChannelParam(command.param))
        return;
    const int index = static_cast<int>(command.param);
    auto& strip = strips[static_cast<std::size_t>(index / kChannelParamCount)];
    const double v = command.value;
    switch (static_cast<ChannelParam>(index % kChannelParamCount))
    {
        case ChannelParam::Trim:
            strip.setTrimDb(v);
            break;
        case ChannelParam::EqLow:
            strip.setBandDb(ChannelStrip::Low, v);
            break;
        case ChannelParam::EqMid:
            strip.setBandDb(ChannelStrip::Mid, v);
            break;
        case ChannelParam::EqHigh:
            strip.setBandDb(ChannelStrip::High, v);
            break;
        case ChannelParam::Filter:
            strip.setFilter(v);
            break;
        case ChannelParam::FilterResonance:
            strip.setFilterResonance(v);
            break;
        case ChannelParam::Fader:
            strip.setFader(v);
            break;
        case ChannelParam::EqMode:
            strip.setEqMode(v >= 0.5 ? ChannelStrip::EqMode::Isolator : ChannelStrip::EqMode::Eq);
            break;
        case ChannelParam::FaderCurve:
            strip.setFaderCurve(v >= 1.5   ? ChannelStrip::FaderCurve::Sharp
                                : v >= 0.5 ? ChannelStrip::FaderCurve::Linear
                                           : ChannelStrip::FaderCurve::Smooth);
            break;
        case ChannelParam::CrossfaderAssign:
        {
            auto& m = mixer[static_cast<std::size_t>(index / kChannelParamCount)];
            m.assign = v >= 1.5 ? CrossfaderAssign::B : v >= 0.5 ? CrossfaderAssign::Thru : CrossfaderAssign::A;
            m.crossfaderGain.setTarget(crossfaderGain(m.assign, crossfaderPosition, crossfaderCurve));
            break;
        }
        case ChannelParam::Cue:
        {
            auto& m = mixer[static_cast<std::size_t>(index / kChannelParamCount)];
            m.cue = v >= 0.5;
            m.cueSend.setTarget(m.cue ? 1.0 : 0.0);
            break;
        }
        case ChannelParam::Count:
            break;
    }
}

void AudioEngine::renderDecks(int numSamples) noexcept
{
    // Deck -> channel strip (pre-fader meter, cue send) -> fader -> crossfader -> master bus.
    float* l = deckScratch.getWritePointer(0);
    float* r = deckScratch.getWritePointer(1);
    for (std::size_t i = 0; i < decks.size(); ++i)
    {
        auto& slot = decks[i];
        auto& m = mixer[i];
        if (slot.deck.track() == nullptr)
        {
            m.meter.processSilence(numSamples);
            m.crossfaderGain.skip(numSamples);
            m.cueSend.skip(numSamples);
            continue;
        }
        const double before = slot.deck.position();
        renderDeckWithActions(slot, l, r, numSamples);
        updateSlicer(slot);
        renderMetronome(i, before, slot.deck.position(), numSamples);
        strips[i].processPreFader(l, r, numSamples);
        processFx(static_cast<FxTarget>(i), l, r, numSamples); // FX units on this channel (P3-08)
        m.meter.process(l, r, numSamples);
        if (m.cueSend.isRamping() || m.cueSend.value() > 0.0)
        {
            float* cl = cueBus.getWritePointer(0);
            float* cr = cueBus.getWritePointer(1);
            for (int s = 0; s < numSamples; ++s)
            {
                const auto g = static_cast<float>(m.cueSend.next());
                cl[s] += g * l[s];
                cr[s] += g * r[s];
            }
        }
        strips[i].applyFader(l, r, numSamples);
        if (m.crossfaderGain.isRamping() || m.crossfaderGain.value() != 1.0)
            for (int s = 0; s < numSamples; ++s)
            {
                const auto g = static_cast<float>(m.crossfaderGain.next());
                l[s] *= g;
                r[s] *= g;
            }
        masterBus.addFrom(0, 0, l, numSamples);
        masterBus.addFrom(1, 0, r, numSamples);
    }
}

void AudioEngine::releaseBuffer(BufferId id) noexcept
{
    if (id == 0)
        return;
    if (releasedBuffers.tryPush(id))
        return;
    if (numPendingReleases < kMaxPendingReleases)
        pendingReleases[static_cast<std::size_t>(numPendingReleases++)] = id;
    // else: the message thread hasn't drained 256 + 16 releases; the buffer stays in the registry (leaks until
    // shutdown) rather than being freed while it might still be in use.
}

void AudioEngine::retryPendingReleases() noexcept
{
    int kept = 0;
    for (int i = 0; i < numPendingReleases; ++i)
    {
        const auto id = pendingReleases[static_cast<std::size_t>(i)];
        if (!releasedBuffers.tryPush(id))
            pendingReleases[static_cast<std::size_t>(kept++)] = id;
    }
    numPendingReleases = kept;
}

void AudioEngine::renderTestTone(int numSamples) noexcept
{
    if (!tone.on && !tone.gain.isSmoothing() && tone.gain.getCurrentValue() == 0.0f)
        return; // silent: nothing to add

    auto& bus = tone.bus == Bus::Cue ? cueBus : masterBus;
    float* left = bus.getWritePointer(0);
    float* right = bus.getWritePointer(1);
    for (int i = 0; i < numSamples; ++i)
    {
        const float sample = static_cast<float>(std::sin(tone.phase)) * tone.gain.getNextValue();
        tone.phase += tone.phaseIncrement;
        if (tone.phase >= 2.0 * std::numbers::pi)
            tone.phase -= 2.0 * std::numbers::pi;
        left[i] += sample * tone.gainLeft;
        right[i] += sample * tone.gainRight;
    }
}

void AudioEngine::publish(int numChannels, int numSamples, double elapsedSeconds) noexcept
{
    const double budget = numSamples / sampleRate;
    const auto load = budget > 0.0 ? static_cast<float>(elapsedSeconds / budget) : 0.0f;

    ++current.blockCounter;
    current.blockSize = numSamples;
    current.sampleRate = sampleRate;
    current.hostTimeSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
    current.cpuLoad += kLoadSmoothing * (load - current.cpuLoad);
    current.cpuLoadPeak = std::max(load, current.cpuLoadPeak * kPeakDecay);
    if (load > 1.0f)
        ++current.xruns;
    current.outputChannels = numChannels;
    current.cueRouted = cueIsRouted(layout, numChannels);
    current.buffersAwaitingRelease = static_cast<std::uint32_t>(numPendingReleases);
    {
        int kept = 0; // replaced effects that didn't fit into the queue: try again
        for (int i = 0; i < numPendingEffects; ++i)
        {
            auto* e = pendingEffects[static_cast<std::size_t>(i)];
            if (!releasedEffects.tryPush(e))
                pendingEffects[static_cast<std::size_t>(kept++)] = e;
        }
        numPendingEffects = kept;
        current.effectsAwaitingRelease = static_cast<std::uint32_t>(numPendingEffects);
        for (int u = 0; u < kFxUnits; ++u)
        {
            const auto& unit = fxUnits[static_cast<std::size_t>(u)];
            auto& fs = current.fx[static_cast<std::size_t>(u)];
            fs.target = fxTargets[static_cast<std::size_t>(u)];
            fs.mode = unit.mode() == dsp::fx::FxUnit::Mode::Single ? 1 : 0;
            for (int s = 0; s < kFxSlots; ++s)
            {
                const auto& slot = unit.slot(s);
                auto& ss = fs.slots[static_cast<std::size_t>(s)];
                ss.hasEffect = slot.effect() != nullptr;
                ss.on = slot.isOn();
                ss.stage = static_cast<std::uint8_t>(slot.stage());
                ss.wet = slot.wet();
                const float b = fxBeats[static_cast<std::size_t>(u)][static_cast<std::size_t>(s)];
                ss.beats = b > 0.0f ? b : 1.0f;
            }
        }
    }
    for (std::size_t i = 0; i < decks.size(); ++i)
    {
        const auto& slot = decks[i];
        const auto& d = slot.deck;
        auto& s = current.decks[i];
        s.loaded = d.track() != nullptr;
        s.bufferId = slot.id;
        s.lengthSamples = d.length();
        s.playing = d.isPlaying();
        s.cuePreview = d.transport() == Deck::Transport::CuePreview;
        s.atEnd = d.isAtEnd();
        s.positionSamples = d.position();
        s.cuePointSamples = d.cuePoint();
        s.rate = d.currentRate();
        s.tempoPercent = d.tempoPercent();
        s.tempoRangePercent = d.tempoRange();
        s.pitchBendPercent = d.pitchBendPercent();
        s.keyLock = d.keyLock();
        s.keyShift = d.keyShift();
        s.scratching = d.isScratching();
        s.transposeSemis = static_cast<std::int8_t>(d.keyShift());
        s.hasGrid = !slot.grid.empty();
        s.gridLocked = slot.grid.locked;
        s.metronome = slot.metronome;
        s.synced = slot.synced;
        s.master = clock.sourceDeck() == static_cast<int>(i);
        s.phaseErrorMs = slot.phaseErrorMs;
        s.queuedActions = slot.queuedCount;
        for (std::size_t c = 0; c < kHotCues; ++c)
        {
            s.hotCueStart[c] = slot.hotCues[c].set ? slot.hotCues[c].start : -1.0;
            s.hotCueLength[c] = slot.hotCues[c].set ? slot.hotCues[c].length : 0.0;
        }
        s.hotCuePreview = slot.hotCuePreview;
        s.loopActive = d.loopActive();
        s.loopStart = d.loopStart();
        s.loopEnd = d.loopEnd();
        s.loopBeats = !slot.grid.empty() && d.loopEnd() > d.loopStart()
                          ? slot.grid.beatAt(d.loopEnd()) - slot.grid.beatAt(d.loopStart())
                          : 0.0;
        s.loopInSet = slot.loopInPending;
        s.loopIn = slot.loopInPoint;
        s.autoLoopBeats = slot.autoLoopBeats;
        s.slip = d.slipOn();
        s.slipping = d.isSlipping();
        s.slipPositionSamples = d.slipPosition();
        s.reverse = d.isReverse();
        s.stopping = static_cast<std::uint8_t>(d.stopKindNow());
        s.slicerOn = slot.slicerOn && !slot.grid.empty() && slot.slicerStartBeat > -1e8;
        s.slicerBeats = slot.slicerBeats;
        s.slicerRolling = slot.slicerRolling;
        s.slicerCurrent = -1;
        if (s.slicerOn)
        {
            s.slicerStart = slot.grid.sampleAt(slot.slicerStartBeat);
            s.slicerEnd = slot.grid.sampleAt(slot.slicerStartBeat + slot.slicerBeats);
            const double into = (slot.grid.beatAt(d.slipPosition()) - slot.slicerStartBeat) / (slot.slicerBeats / 8.0);
            s.slicerCurrent = into >= 0.0 && into < 8.0 ? static_cast<int>(into) : -1;
        }
        if (s.hasGrid)
        {
            s.beat = slot.grid.beatAt(s.positionSamples);
            s.barBeat = slot.grid.barBeatOf(static_cast<long long>(std::floor(s.beat + 1e-6)));
            s.gridBpm = slot.grid.bpmAt(s.positionSamples);
        }
    }
    for (std::size_t i = 0; i < strips.size(); ++i)
    {
        const auto& strip = strips[i];
        auto& c = current.channels[i];
        c.trimDb = static_cast<float>(strip.trimDb());
        for (int b = 0; b < ChannelStrip::kBands; ++b)
            c.eqDb[static_cast<std::size_t>(b)] = static_cast<float>(strip.bandDb(b));
        c.filter = static_cast<float>(strip.filter());
        c.filterResonance = static_cast<float>(strip.filterResonance());
        c.fader = static_cast<float>(strip.fader());
        c.eqMode = static_cast<std::uint8_t>(strip.eqMode());
        c.faderCurve = static_cast<std::uint8_t>(strip.faderCurve());
        const auto& m = mixer[i];
        c.crossfaderAssign = static_cast<std::uint8_t>(m.assign);
        c.cue = m.cue;
        c.peakL = m.meter.peakLeft();
        c.peakR = m.meter.peakRight();
        c.rms = m.meter.rms();
    }
    current.crossfader = static_cast<float>(crossfaderPosition);
    current.crossfaderCurve = static_cast<std::uint8_t>(crossfaderCurve);
    current.masterGainDb = static_cast<float>(masterGainDb);
    current.masterPeakL = masterMeter.peakLeft();
    current.masterPeakR = masterMeter.peakRight();
    current.masterRms = masterMeter.rms();
    current.limiterGainReductionDb = blockGainReductionDb;
    blockGainReductionDb = 0.0f;
    current.masterLatencySamples = limiter.latencySamples();
    current.cueMix = static_cast<float>(cueMix);
    current.headphoneLevelDb = static_cast<float>(headphoneLevelDb);
    current.cuePeakL = cueMeter.peakLeft();
    current.cuePeakR = cueMeter.peakRight();
    current.sampleClock = clock.blockEnd();
    current.masterBpm = clock.bpm();
    current.masterBeat = clock.beatAtBlockEnd();
    current.masterSource = static_cast<std::int8_t>(clock.sourceDeck());
    current.clockMode = static_cast<std::uint8_t>(clock.mode());
    current.internalBpm = clock.internalBpm();
    current.quantize = quantizeOn;
    current.quantizeBeats = static_cast<float>(quantizeBeats);

    state.write(current);
}

} // namespace devildj::engine
