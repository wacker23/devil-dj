#include "engine/AudioEngine.h"

#include "dsp/fx/BeatFx.h"

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
    deckScratch.setSize(4, capacity, false, true, false); // 2, 3: a deck's stems past the effects (P6-04)
    for (auto& slot : decks)
        slot.deck.prepare(sampleRate, capacity);
    clock.prepare(sampleRate);
    cueOutput.setWriter(sampleRate, capacity);
    for (auto& c : colorFx) // P3-10
        c.prepare(sampleRate, capacity);
    for (auto& rf : releaseFx) // P3-11
        rf.prepare(sampleRate, capacity);
    for (auto& channel : padFx) // P3-12
        for (auto& slot : channel)
            slot.prepare(sampleRate, capacity);
    for (auto& strip : strips)
        strip.prepare(sampleRate, capacity);
    for (auto& strip : dryStrips) // P6-04
        strip.prepare(sampleRate, capacity);
    sampler.prepare(sampleRate, capacity); // P3-13
    // The Beat Maker (P4-10): its voices and its send effects (a 3/4-beat echo and the reverb, fully wet).
    for (auto* module : {beatMaker.get(), codeDeck.get()}) // the Beat Maker's voices (P4-10), the code deck's (P5-12)
    {
        module->voices.prepare(sampleRate, capacity);
        module->scratch.setSize(6, capacity, false, true, false);
        if (module->delay == nullptr)
        {
            module->delay = dsp::fx::makeEffect(dsp::fx::EffectType::Echo);
            module->reverb = dsp::fx::makeEffect(dsp::fx::EffectType::Reverb);
        }
        module->delay->prepare(sampleRate, capacity);
        module->delay->setBeats(0.75); // 3/16 of a bar (CODEBEAT_SPEC 5.6)
        module->reverb->prepare(sampleRate, capacity);
        module->reverb->setBeats(1.0);
        module->due.clear();
    }
    transitionFxBus.setSize(4, capacity, false, true, false); // P4-18
    sequencer.reset();                      // P3-15
    if (!tapPrepared) // P3-16: once, for the fastest rate (the recorder thread may be reading it later)
    {
        tap.prepare(192000.0, 2.0);
        tapPrepared = true;
    }
    samplerPendingBeat.fill(-1.0);
    samplerPending = false;
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
    transitionFxBus.clear(0, numSamples); // P4-18: this block's sends
    transitionFxFed = {false, false};

    advanceClock(numSamples); // before the decks render: the clock reads their playheads at the block start
    renderDecks(numSamples);
    renderMaster(numSamples);   // master gain
    mixHeadphones(numSamples);  // cue bus + master -> headphone level
    renderTestTone(numSamples); // straight onto its bus (not into the headphone mix)
    finishBuses(numSamples);    // limiters and meters
    if (tap.isEnabled()) // the mix recording (P3-16): what goes out, after the limiters
    {
        const bool phones = tap.getSource() == RecordTap::Source::Headphones;
        tap.write((phones ? cueBus : masterBus).getReadPointer(0), (phones ? cueBus : masterBus).getReadPointer(1),
                  numSamples);
    }
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
    runTransition(); // P3-17: before the decks follow the clock and render
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

void AudioEngine::apply(const LoadStems& command) noexcept
{
    if (command.deck >= kMaxDecks)
    {
        releaseBuffer(command.bufferId);
        return;
    }
    auto& slot = decks[command.deck];
    if (slot.hasPending || slot.deck.track() == nullptr || slot.deck.track() != command.track ||
        command.stems == nullptr || command.stems->track != command.track)
    {
        releaseBuffer(command.bufferId); // late: that track isn't (or won't stay) on the deck
        return;
    }
    slot.deck.setStems(command.stems);
    if (slot.stemsId != 0 && slot.stemsId != command.bufferId)
        releaseBuffer(slot.stemsId); // replaced: the deck reads the new ones from the next render
    slot.stemsId = command.bufferId;
}

void AudioEngine::apply(const SetStem& command) noexcept
{
    if (command.deck >= kMaxDecks)
        return;
    auto& deck = decks[command.deck].deck;
    const int stem = command.stem;
    const bool on = command.value > 0.5f;
    switch (command.param)
    {
        case StemParam::Level:
            deck.setStemLevel(stem, command.value);
            break;
        case StemParam::Mute:
            deck.setStemMute(stem, on);
            break;
        case StemParam::Solo:
            deck.setStemSolo(stem, on);
            break;
        case StemParam::Fx:
            deck.setStemFx(stem, on);
            break;
        case StemParam::Reset:
            deck.resetStems();
            break;
    }
}

void AudioEngine::apply(const SetSwapLoop& command) noexcept
{
    if (command.deck >= kMaxDecks || command.slot >= Deck::kSwapSlots)
    {
        releaseBuffer(command.bufferId);
        return;
    }
    auto& slot = decks[command.deck];
    slot.deck.setSwapLoop(command.slot, command.loop, command.beats);
    auto& id = slot.swapIds[command.slot];
    if (id != 0 && id != command.bufferId)
        releaseBuffer(id); // the deck reads the new one from the next render
    id = command.loop != nullptr ? command.bufferId : 0;
    if (command.loop == nullptr && command.bufferId != 0)
        releaseBuffer(command.bufferId);
}

void AudioEngine::apply(const DrumSwap& command) noexcept
{
    if (command.deck >= kMaxDecks || command.slot >= Deck::kSwapSlots)
        return;
    auto& slot = decks[command.deck];
    auto& deck = slot.deck;
    if (deck.track() == nullptr || deck.stems() == nullptr)
        return; // nothing to swap the drums of
    // From where things will be once a swap already waiting has happened.
    DeckSlot::QueuedAction* waiting = nullptr;
    for (int k = 0; k < slot.queuedCount; ++k)
        if (slot.queued[static_cast<std::size_t>(k)].kind == DeckSlot::QueuedAction::Kind::Swap)
            waiting = &slot.queued[static_cast<std::size_t>(k)];
    const std::uint8_t base = waiting != nullptr ? waiting->swapMask : deck.swapMask();
    const auto bit = static_cast<std::uint8_t>(1u << command.slot);
    std::uint8_t mask = 0;
    switch (command.op)
    {
        case DrumSwapOp::Single:
            mask = base == bit ? std::uint8_t{0} : bit;
            break;
        case DrumSwapOp::Layer:
            mask = static_cast<std::uint8_t>(base ^ bit);
            break;
        case DrumSwapOp::Off:
            mask = 0;
            break;
    }
    mask = static_cast<std::uint8_t>(mask & deck.swapLoaded());

    // The bar: the grid's next downbeat far enough ahead for the fade (and key lock's read-ahead) to end on it.
    const auto& grid = slot.grid;
    const double pos = deck.position();
    const double rate = std::max(0.05, deck.rate());
    const double lead = Deck::kSwapFadeSeconds * sampleRate * rate + deck.readAhead();
    const bool moving = deck.isPlaying() && !deck.isReverse() && !deck.isStopping() && !deck.isScratching();
    if (grid.empty())
    {
        if (waiting != nullptr)
            waiting->swapMask = mask;
        else
            deck.setSwap(mask, pos, sampleRate / 2.0); // no grid: at once, 120 BPM
        return;
    }
    const auto barAtOrAfter = [&grid](double sample)
    {
        auto b = static_cast<long long>(std::ceil(grid.beatAt(sample) - 1e-9));
        for (int k = 0; k < 8 && grid.barBeatOf(b) != 1; ++k)
            ++b;
        return b;
    };
    if (!moving)
    {
        if (waiting != nullptr)
            waiting->swapMask = mask;
        else
        {
            // At once; new loops count from the bar at or before the playhead (so they're in step once it plays).
            auto b = static_cast<long long>(std::floor(grid.beatAt(pos) + 1e-9));
            for (int k = 0; k < 8 && grid.barBeatOf(b) != 1; ++k)
                --b;
            deck.setSwap(mask, grid.sampleAt(static_cast<double>(b)),
                         grid.sampleAt(static_cast<double>(b + 1)) - grid.sampleAt(static_cast<double>(b)));
        }
        return;
    }
    const long long b = barAtOrAfter(pos + lead + rate * capacity);
    const double bar = grid.sampleAt(static_cast<double>(b));
    DeckSlot::QueuedAction a;
    a.kind = DeckSlot::QueuedAction::Kind::Swap;
    a.at = bar - lead;
    a.to = bar;
    a.swapMask = mask;
    a.samplesPerBeat = grid.sampleAt(static_cast<double>(b + 1)) - bar;
    if (waiting != nullptr)
        *waiting = a;
    else
        queue(slot, a);
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
        if (slot.stemsId != 0)
            releaseBuffer(slot.stemsId); // its stems too (P6-04)
        slot.stemsId = 0;
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
    if (auto& rf = releaseFx[static_cast<std::size_t>(&slot - decks.data())]; rf.finished())
        rf.reset(); // a track that ended on a release FX plays normally again (P3-11)
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
                    if (a.kind != DeckSlot::QueuedAction::Kind::Jump || a.loopLength > 0.0) // plays, swaps, loops
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
        if (a.kind != DeckSlot::QueuedAction::Kind::Jump) // plays and Drum Swaps (P6-05) stay
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
        case FxOp::TapBpm:
            fxManualBpm[command.unit] = command.value > 0.0f ? std::clamp<double>(command.value, 20.0, 300.0) : 0.0;
            break;
    }
}

dsp::fx::BeatContext AudioEngine::beatContext() const noexcept
{
    dsp::fx::BeatContext beat;
    beat.bpm = clock.bpm();
    beat.beatAtBlockStart = clock.beatAtBlockStart();
    beat.beatsPerSample = clock.beatsPerSample();
    return beat;
}

bool AudioEngine::installPadEffect(int deck, int pad, std::unique_ptr<dsp::fx::Effect> effect)
{
    if (deck < 0 || deck >= kMaxChannels || pad < 0 || pad >= kPadFxSlots)
        return false;
    dsp::fx::Effect* raw = effect.get();
    if (effect != nullptr)
    {
        effect->prepare(sampleRate, preparedBlockSize);
        ownedEffects.push_back(std::move(effect));
    }
    if (send(PadFxSetEffect{static_cast<DeckId>(deck), static_cast<std::uint8_t>(pad), raw}))
        return true;
    if (raw != nullptr)
        ownedEffects.pop_back();
    return false;
}

void AudioEngine::apply(const PadFxSetEffect& command) noexcept
{
    if (command.deck >= kMaxChannels || command.pad >= kPadFxSlots)
        return;
    auto& slot = padFx[command.deck][command.pad];
    retireEffect(slot.effect());
    slot.setEffect(command.effect);
}

void AudioEngine::apply(const PadFxCommand& command) noexcept
{
    if (command.deck >= kMaxChannels || command.pad >= kPadFxSlots)
        return;
    auto& slot = padFx[command.deck][command.pad];
    switch (command.op)
    {
        case FxOp::On:
            slot.setOn(command.value != 0.0f);
            break;
        case FxOp::Wet:
            slot.setWet(command.value);
            break;
        case FxOp::Beats:
            if (slot.effect() != nullptr)
                slot.effect()->setBeats(std::clamp(command.value, 1.0f / 16.0f, 16.0f));
            break;
        case FxOp::Param:
            if (slot.effect() != nullptr)
                slot.effect()->setParam(command.index, std::clamp(command.value, 0.0f, 1.0f));
            break;
        default:
            break;
    }
}

void AudioEngine::apply(const SamplerLoad& command) noexcept
{
    if (command.slot >= Sampler::kSlots)
    {
        if (command.id != 0)
            releaseBuffer(command.id);
        return;
    }
    samplerPendingBeat[command.slot] = -1.0;
    const BufferId old = sampler.load(command.slot, command.buffer, command.id, command.bpm);
    if (old != 0 && old != command.id)
        releaseBuffer(old); // nothing plays it any more
}

void AudioEngine::apply(const SamplerPad& command) noexcept
{
    if (command.slot >= Sampler::kSlots)
        return;
    if (command.release)
    {
        samplerPendingBeat[command.slot] = -1.0; // a gate let go before its beat: never starts
        sampler.releasePad(command.slot);
        if (sampler.slot(command.slot).mode == Sampler::Mode::Gate)
            sequencer.record(command.slot, true, clock.beatAtBlockEnd()); // (only while recording)
        return;
    }
    const auto& slot = sampler.slot(command.slot);
    const bool stopping = slot.mode == Sampler::Mode::Loop && sampler.isPlaying(command.slot);
    if (quantizeOn && !stopping) // on the master clock's next grid point
    {
        samplerPendingBeat[command.slot] =
            std::ceil(clock.beatAtBlockEnd() / quantizeBeats - 1e-9) * quantizeBeats;
        samplerPending = true;
        sequencer.record(command.slot, false, samplerPendingBeat[command.slot]); // where it will sound
        return;
    }
    sampler.press(command.slot, 0);
    sequencer.record(command.slot, false, clock.beatAtBlockEnd());
}

void AudioEngine::apply(const SequencerCommand& command) noexcept
{
    switch (command.op)
    {
        case SequencerOp::Clear:
            sequencer.clear(static_cast<int>(command.value));
            break;
        case SequencerOp::Bars:
            sequencer.setBars(static_cast<int>(command.value));
            break;
        case SequencerOp::Play:
            sequencer.setPlaying(command.value != 0.0f);
            break;
        case SequencerOp::Record:
            if (command.value != 0.0f)
                sequencer.setRecording(true);
            else
                sequencer.setRecording(false); // plays on
            break;
        case SequencerOp::Erase:
            sequencer.erase();
            break;
        case SequencerOp::EraseSlot:
            sequencer.eraseSlot(command.slot);
            break;
        case SequencerOp::Mute:
            sequencer.setMuted(command.slot, command.value != 0.0f);
            break;
        case SequencerOp::Metronome:
            sequencerMetronome = command.value != 0.0f;
            break;
    }
}

void AudioEngine::apply(const SequencerEvents& command) noexcept
{
    for (int i = 0; i < std::min<int>(command.count, SequencerEvents::kChunk); ++i)
        sequencer.add(command.events[static_cast<std::size_t>(i)]);
}

void AudioEngine::renderSequencerClick(int numSamples) noexcept
{
    // While recording with the metronome on: a click on every master beat, into the headphones.
    if (sequencerMetronome && sequencer.recording() && clock.beatsPerSample() > 0.0)
    {
        const double a = clock.beatAtBlockStart(), b = clock.beatAtBlockEnd();
        const double k = std::ceil(a - 1e-9);
        if (k < b)
        {
            const bool downbeat = std::fmod(k, 4.0) == 0.0;
            seqClickDelay = static_cast<int>(std::floor((k - a) / clock.beatsPerSample()));
            seqClickPosition = 0;
            seqClickLength = static_cast<int>(0.015 * sampleRate);
            seqClickIncrement = 2.0 * std::numbers::pi * (downbeat ? 2000.0 : 1000.0) / sampleRate;
            seqClickLevel = downbeat ? 0.5f : 0.35f;
            seqClickActive = true;
        }
    }
    if (!seqClickActive)
        return;
    float* cl = cueBus.getWritePointer(0);
    float* cr = cueBus.getWritePointer(1);
    const double decay = 1.0 / (0.003 * sampleRate);
    for (int s = std::max(0, seqClickDelay); s < numSamples && seqClickPosition < seqClickLength; ++s, ++seqClickPosition)
    {
        const double p = seqClickPosition;
        const auto v = static_cast<float>(seqClickLevel * std::exp(-p * decay) * std::sin(p * seqClickIncrement));
        cl[s] += v;
        cr[s] += v;
    }
    seqClickDelay = 0;
    if (seqClickPosition >= seqClickLength)
        seqClickActive = false;
}

// ---- auto-transition (P3-17) ----

void AudioEngine::setLowEq(DeckId deck, bool kill) noexcept
{
    apply(SetParam{channelParam(deck, ChannelParam::EqLow), kill ? -1000.0f : 0.0f});
}

bool AudioEngine::startTransition(StartTransition command, std::shared_ptr<const TransitionCurves> curves)
{
    command.curves = curves.get();
    // P4-18: its FX lanes' effects, made and prepared here (a new one replaces the one before).
    if (curves != nullptr)
        for (int side = 0; side < 2; ++side)
            if (const int type = curves->fx[static_cast<std::size_t>(side)]; type >= 0 && type < dsp::fx::kEffectTypeCount)
            {
                auto effect = dsp::fx::makeEffect(static_cast<dsp::fx::EffectType>(type));
                effect->prepare(sampleRate, preparedBlockSize);
                effect->setBeats(curves->fxBeats);
                auto* raw = effect.get();
                ownedEffects.push_back(std::move(effect));
                if (!send(TransitionFxSetEffect{static_cast<std::uint8_t>(side), raw}))
                    ownedEffects.pop_back();
            }
    // Kept until four more have been started: by then the audio thread has long let go of it (a new transition
    // ends the one before).
    heldCurves[static_cast<std::size_t>(heldNext)] = std::move(curves);
    heldNext = (heldNext + 1) % static_cast<int>(heldCurves.size());
    return send(command);
}

void AudioEngine::apply(const StartTransition& command) noexcept
{
    if (transition.active)
        endTransition(false);
    if (command.from >= 2 || command.to >= 2 || command.from == command.to)
        return;
    auto& to = decks[command.to];
    if (decks[command.from].deck.track() == nullptr || to.deck.track() == nullptr || to.hasPending)
        return;
    auto& t = transition;
    t = {};
    t.active = true;
    t.from = command.from;
    t.to = command.to;
    if (command.curves != nullptr) // P4-14: the lanes, any length
    {
        t.curves = command.curves;
        t.beats = std::max(1.0, static_cast<double>(command.curves->beats));
        t.startBeat = command.startBeat >= 0.0 ? command.startBeat : std::ceil(clock.beatAtBlockEnd() - 1e-9);
        // What the two channels are now (put back at the end), then both THRU: the lanes do the levels, the
        // crossfader goes to the incoming deck's side at the end.
        for (int k = 0; k < 2; ++k)
        {
            const auto d = static_cast<std::size_t>(k == 0 ? t.from : t.to);
            auto& sv = t.saved[static_cast<std::size_t>(k)];
            for (int b = 0; b < ChannelStrip::kBands; ++b)
                sv.eq[static_cast<std::size_t>(b)] = strips[d].bandDb(b);
            sv.filter = filterKnob[d];
            sv.assign = mixer[d].assign;
        }
        to.synced = true;
        if (!to.deck.isPlaying())
            queue(to, {DeckSlot::QueuedAction::Kind::Play, t.startBeat, 0.0});
        return;
    }
    t.beats = command.beats >= 24.0f ? 32.0 : command.beats >= 12.0f ? 16.0 : 8.0;
    t.style = command.style;
    t.startBeat = command.startBeat >= 0.0 ? command.startBeat
                                           : std::ceil(clock.beatAtBlockEnd() / 16.0 - 1e-9) * 16.0;
    // Which way the crossfader goes: from the outgoing deck's side to the incoming one's; without two sides, the
    // outgoing deck's fader comes down instead.
    const auto sideFrom = mixer[command.from].assign, sideTo = mixer[command.to].assign;
    t.faderMode = sideFrom == CrossfaderAssign::Thru || sideTo == CrossfaderAssign::Thru || sideFrom == sideTo;
    t.crossfaderTo = sideTo == CrossfaderAssign::A ? 0.0 : 1.0;
    t.fromFader = strips[command.from].fader();
    // The incoming deck: synced, its lows out, started on the start beat (from where it is cued).
    to.synced = true;
    setLowEq(command.to, true);
    if (!to.deck.isPlaying())
        queue(to, {DeckSlot::QueuedAction::Kind::Play, t.startBeat, 0.0});
}

void AudioEngine::apply(const CancelTransition&) noexcept
{
    if (transition.active)
        endTransition(false);
}

void AudioEngine::endTransition(bool finished) noexcept
{
    auto& t = transition;
    if (!t.active)
        return;
    t.active = false;
    auto& to = decks[t.to];
    if (!t.started) // cancelled before it began: the incoming deck doesn't start
    {
        int kept = 0;
        for (int k = 0; k < to.queuedCount; ++k)
            if (to.queued[static_cast<std::size_t>(k)].kind != DeckSlot::QueuedAction::Kind::Play)
                to.queued[static_cast<std::size_t>(kept++)] = to.queued[static_cast<std::size_t>(k)];
        to.queuedCount = kept;
    }
    if (t.curves != nullptr) // P4-14: both channels as they were; finished, the crossfader on the incoming side
    {
        if (finished)
            decks[t.from].deck.pause();
        for (int k = 0; k < 2; ++k)
        {
            const auto d = k == 0 ? t.from : t.to;
            const auto& sv = t.saved[static_cast<std::size_t>(k)];
            apply(SetParam{channelParam(d, ChannelParam::EqLow), static_cast<float>(sv.eq[0])});
            apply(SetParam{channelParam(d, ChannelParam::EqMid), static_cast<float>(sv.eq[1])});
            apply(SetParam{channelParam(d, ChannelParam::EqHigh), static_cast<float>(sv.eq[2])});
            apply(SetParam{channelParam(d, ChannelParam::Filter), static_cast<float>(sv.filter)});
            apply(SetParam{channelParam(d, ChannelParam::CrossfaderAssign), static_cast<float>(static_cast<int>(sv.assign))});
            auto& m = mixer[d];
            m.autoGain = false;
            m.autoGainStart = m.autoGainEnd = 1.0f;
        }
        for (const auto d : {t.from, t.to}) // the FX lanes stop sending (the effects' tails go on)
            mixer[d].transitionSend = -1;
        if (transitionLoop) // the loop out ends with it
        {
            decks[t.from].deck.exitLoop();
            transitionLoop = false;
        }
        const auto side = t.saved[1].assign;
        if (finished && side != CrossfaderAssign::Thru)
            apply(SetParam{ParamId::Crossfader, side == CrossfaderAssign::A ? 0.0f : 1.0f});
        t.curves = nullptr;
        return;
    }
    if (finished && t.style != TransitionStyle::EchoOut)
        decks[t.from].deck.pause(); // the echo out ends it itself
    if (!finished && releaseFx[t.from].stage() == dsp::fx::ReleaseFx::Stage::Held)
        releaseFx[t.from].reset();
    setLowEq(t.from, false);
    setLowEq(t.to, false);
    if (t.style == TransitionStyle::FilterSweep)
        apply(SetParam{channelParam(t.from, ChannelParam::Filter), 0.5f});
    if (t.faderMode && finished)
        apply(SetParam{channelParam(t.from, ChannelParam::Fader), static_cast<float>(t.fromFader)});
}

void AudioEngine::runCurves() noexcept
{
    auto& t = transition;
    const double b0 = clock.beatAtBlockStart() - t.startBeat, b1 = clock.beatAtBlockEnd() - t.startBeat;
    if (b0 < 0.0 && b1 <= 0.0)
        return; // waiting for its beat
    if (!t.started)
    {
        t.started = true;
        t.fromScale = static_cast<float>(mixer[t.from].crossfaderGain.value());
        for (const auto d : {t.from, t.to}) // the lanes do the levels from here
            apply(SetParam{channelParam(d, ChannelParam::CrossfaderAssign), static_cast<float>(static_cast<int>(CrossfaderAssign::Thru))});
        // Loop out (P4-18): the outgoing deck loops the last loopBeats before the transition's start beat.
        auto& slot = decks[t.from];
        if (t.curves->loopBeats > 0.0f && !slot.grid.empty())
        {
            const double here = slot.grid.beatAt(slot.deck.position());
            const double end = std::round(here - std::max(0.0, b0));
            const double a = slot.grid.sampleAt(end - t.curves->loopBeats), e = slot.grid.sampleAt(end);
            if (a >= 0.0 && e > a)
            {
                slot.deck.setLoop(a, e);
                transitionLoop = true;
            }
        }
    }
    const auto& c = *t.curves;
    t.lanesUsed = 0;
    for (int k = 0; k < 2; ++k)
    {
        const auto d = k == 0 ? t.from : t.to;
        const auto& lanes = k == 0 ? c.from : c.to;
        for (int l = 0; l < kLaneCount; ++l)
        {
            const auto& lane = lanes[static_cast<std::size_t>(l)];
            const auto slot = static_cast<std::size_t>(k * kLaneCount + l);
            if (!lane.used())
            {
                t.lanes[slot] = neutralValue(static_cast<Lane>(l));
                if (k == 0 && static_cast<Lane>(l) == Lane::Volume) // no volume lane: as loud as it was
                {
                    auto& m = mixer[d];
                    m.autoGain = true;
                    m.autoGainStart = m.autoGainEnd = t.fromScale;
                }
                continue;
            }
            t.lanesUsed = static_cast<std::uint16_t>(t.lanesUsed | (1u << slot));
            const float v = evaluate(lane, std::max(0.0, b0));
            t.lanes[slot] = v;
            switch (static_cast<Lane>(l))
            {
                case Lane::Volume:
                {
                    auto& m = mixer[d];
                    const float scale = k == 0 ? t.fromScale : 1.0f;
                    m.autoGain = true;
                    m.autoGainStart = v * scale;
                    m.autoGainEnd = evaluate(lane, std::max(0.0, b1)) * scale;
                    break;
                }
                case Lane::EqLow:
                case Lane::EqMid:
                case Lane::EqHigh:
                {
                    const auto band = static_cast<int>(l) - static_cast<int>(Lane::EqLow);
                    const auto p = band == 0 ? ChannelParam::EqLow : band == 1 ? ChannelParam::EqMid : ChannelParam::EqHigh;
                    apply(SetParam{channelParam(d, p), v});
                    break;
                }
                case Lane::Filter:
                    apply(SetParam{channelParam(d, ChannelParam::Filter), 0.5f + 0.5f * std::clamp(v, -1.0f, 1.0f)});
                    break;
                case Lane::FxSend: // into this side's effect (P4-18)
                {
                    auto& m = mixer[d];
                    m.transitionSend = c.fx[static_cast<std::size_t>(k)] >= 0 ? k : -1;
                    m.sendStart = v;
                    m.sendEnd = evaluate(lane, std::max(0.0, b1));
                    break;
                }
            }
        }
    }
    if (b0 >= t.beats)
        endTransition(true);
}

void AudioEngine::runTransition() noexcept
{
    auto& t = transition;
    if (!t.active)
        return;
    if (t.curves != nullptr)
    {
        runCurves();
        return;
    }
    const double p = (clock.beatAtBlockStart() - t.startBeat) / t.beats;
    if (p < 0.0)
        return; // waiting for its beat
    if (!t.started)
    {
        t.started = true;
        t.crossfaderFrom = crossfaderPosition;
    }
    const double x = std::min(1.0, p);
    if (t.faderMode)
        apply(SetParam{channelParam(t.from, ChannelParam::Fader), static_cast<float>(t.fromFader * (1.0 - x))});
    else
        apply(SetParam{ParamId::Crossfader, static_cast<float>(t.crossfaderFrom + (t.crossfaderTo - t.crossfaderFrom) * x)});
    if (!t.swapped && x >= 0.5) // the low EQ swap at the middle
    {
        t.swapped = true;
        setLowEq(t.from, true);
        setLowEq(t.to, false);
    }
    if (t.style == TransitionStyle::FilterSweep) // the outgoing deck into a high-pass
        apply(SetParam{channelParam(t.from, ChannelParam::Filter), static_cast<float>(0.5 + 0.45 * x)});
    if (t.style == TransitionStyle::EchoOut)
    {
        if (!t.echoPressed && x >= 0.75)
        {
            t.echoPressed = true;
            releaseFx[t.from].press(dsp::fx::ReleaseFx::Kind::EchoOut, 1.0);
        }
        if (x >= 1.0)
            releaseFx[t.from].release(); // cut: it ends on the echo, then the deck pauses (P3-11)
    }
    if (p >= 1.0)
        endTransition(true);
}

void AudioEngine::apply(const SamplerSet& command) noexcept
{
    switch (command.op)
    {
        case SamplerOp::Mode:
            sampler.setMode(command.slot, static_cast<Sampler::Mode>(std::clamp(static_cast<int>(command.value), 0, 2)));
            break;
        case SamplerOp::Gain:
            sampler.setGain(command.slot, command.value);
            break;
        case SamplerOp::Sync:
            sampler.setSync(command.slot, command.value != 0.0f);
            break;
        case SamplerOp::Stop:
            samplerPendingBeat[std::min<std::size_t>(command.slot, Sampler::kSlots - 1)] = -1.0;
            sampler.stop(command.slot);
            break;
        case SamplerOp::StopAll:
            samplerPendingBeat.fill(-1.0);
            sampler.stopAll();
            break;
    }
}

void AudioEngine::apply(const ReleaseFxCommand& command) noexcept
{
    if (command.deck >= kMaxChannels)
        return;
    auto& rf = releaseFx[command.deck];
    if (command.press && command.kind < dsp::fx::ReleaseFx::kKindCount)
        rf.press(static_cast<dsp::fx::ReleaseFx::Kind>(command.kind), command.beats > 0.0f ? command.beats : 1.0);
    else if (!command.press)
        rf.release();
}

void AudioEngine::apply(const SetColorFx& command) noexcept
{
    if (command.type >= dsp::fx::ColorFx::kTypeCount)
        return;
    const auto type = static_cast<dsp::fx::ColorFx::Type>(command.type);
    if (type == colorType)
        return;
    colorType = type;
    for (std::size_t ch = 0; ch < strips.size(); ++ch)
    {
        // The knob moves over to its new job; the old one goes back to its centre (both over 20 ms).
        strips[ch].setFilter(type == dsp::fx::ColorFx::Type::Filter ? filterKnob[ch] : 0.5);
        colorFx[ch].setType(type);
        colorFx[ch].setKnob(filterKnob[ch]);
    }
}

void AudioEngine::processFx(FxTarget target, float* left, float* right, int numSamples) noexcept
{
    const auto beat = beatContext();
    for (std::size_t u = 0; u < static_cast<std::size_t>(kFxUnits); ++u)
    {
        if (fxTargets[u] != target)
            continue;
        if (fxManualBpm[u] > 0.0) // a tapped tempo: its own beat count
        {
            dsp::fx::BeatContext manual;
            manual.bpm = fxManualBpm[u];
            manual.beatsPerSample = fxManualBpm[u] / (60.0 * sampleRate);
            manual.beatAtBlockStart = fxManualBeat[u];
            fxManualBeat[u] += numSamples * manual.beatsPerSample;
            fxUnits[u].process(left, right, numSamples, manual);
        }
        else
        {
            fxUnits[u].process(left, right, numSamples, beat);
        }
    }
}

void AudioEngine::apply(const JumpBy& command) noexcept
{
    if (command.deck >= kMaxDecks || decks[command.deck].hasPending)
        return;
    auto& deck = decks[command.deck].deck;
    if (deck.track() != nullptr)
        deck.seek(deck.position() + command.seconds * sampleRate);
}

bool AudioEngine::renderDeckWithActions(DeckSlot& slot, float* l, float* r, float* dl, float* dr,
                                        int numSamples) noexcept
{
    int done = 0;
    bool dry = false;
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
            slot.deck.render(l + done, r + done, dl + done, dr + done, due - done);
            dry = dry || slot.deck.dryActive();
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
        else if (action.kind == DeckSlot::QueuedAction::Kind::Swap) // P6-05: the fade starts; it ends on the bar
        {
            slot.deck.setSwap(action.swapMask, action.to, action.samplesPerBeat);
        }
        else
        {
            if (action.loopLength > 0.0) // a hot loop: looping from its start, now that the jump is there
                slot.deck.setLoop(action.to, action.to + action.loopLength); // first: slip's shadow starts here
            slot.deck.seek(action.to + std::max(0.0, overshoot));
        }
    }
    if (done < numSamples)
    {
        slot.deck.render(l + done, r + done, dl + done, dr + done, numSamples - done);
        dry = dry || slot.deck.dryActive();
    }
    return dry;
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
        case ChannelParam::Filter: // the strip's filter, or the color FX (P3-10)
        {
            const auto ch = static_cast<std::size_t>(index / kChannelParamCount);
            filterKnob[ch] = std::clamp(v, 0.0, 1.0);
            if (colorType == dsp::fx::ColorFx::Type::Filter)
                strip.setFilter(filterKnob[ch]);
            else
                colorFx[ch].setKnob(filterKnob[ch]);
            break;
        }
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
        float* dl = deckScratch.getWritePointer(2);
        float* dr = deckScratch.getWritePointer(3);
        const bool dry = renderDeckWithActions(slot, l, r, dl, dr, numSamples);
        updateSlicer(slot);
        renderMetronome(i, before, slot.deck.position(), numSamples);
        if (dry) // stems past the effects (P6-04): their own strip, with the channel's settings
        {
            auto& ds = dryStrips[i];
            const auto& src = strips[i];
            ds.setTrimDb(src.trimDb());
            ds.setEqMode(src.eqMode());
            for (int b = 0; b < ChannelStrip::kBands; ++b)
                ds.setBandDb(b, src.bandDb(b));
            ds.setFilter(src.filter());
            ds.setFilterResonance(src.filterResonance());
            if (!slot.dryWasActive)
                ds.reset(); // fresh state at the settings (the stems ramp in from silence)
        }
        slot.dryWasActive = dry;
        mixChannel(static_cast<int>(i), l, r, numSamples, dry ? dl : nullptr, dry ? dr : nullptr);
        if (releaseFx[i].finished()) // ended on the effect: the deck pauses; back to normal once it is silent
        {
            if (slot.deck.isPlaying())
                slot.deck.pause();
            else if (slot.deck.isSilent())
                releaseFx[i].reset();
        }
    }
    renderSampler(numSamples); // its own channel (P3-13)
    renderVoices(*beatMaker, kBeatMakerChannel, numSamples); // and the Beat Maker's (P4-10)
    renderVoices(*codeDeck, kCodeDeckChannel, numSamples);   // and the code deck's (P5-12)
    processTransitionFx(numSamples); // a transition's send effects and their tails (P4-18)
}

AudioEngine::VoiceModule& AudioEngine::moduleOf(SourceId source) noexcept
{
    return source == SourceId::CodeDeck ? *codeDeck : *beatMaker;
}

void AudioEngine::renderVoices(VoiceModule& b, int channel, int numSamples) noexcept
{
    // The events its producer has sent, then those due in this block - each voice starts on its own sample.
    b.due.drain(b.queue);
    const auto lateBefore = b.due.lateEvents();
    b.due.takeDue(clock.blockStart(), numSamples,
                  [&](const ScheduledEvent& e, int offset) { b.voices.start(e, offset); });
    b.late += b.due.lateEvents() - lateBefore;
    auto& s = b.scratch;
    for (int c = 0; c < 6; ++c)
        juce::FloatVectorOperations::clear(s.getWritePointer(c), numSamples);
    float* l = s.getWritePointer(0);
    float* r = s.getWritePointer(1);
    b.voices.process({l, r, s.getWritePointer(2), s.getWritePointer(3), s.getWritePointer(4), s.getWritePointer(5)},
                     numSamples);
    // The sends: through the echo and the reverb (wet only), back into the dry mix.
    const auto ctx = beatContext();
    b.delay->process(s.getWritePointer(2), s.getWritePointer(3), numSamples, ctx);
    b.reverb->process(s.getWritePointer(4), s.getWritePointer(5), numSamples, ctx);
    for (int c = 2; c < 6; ++c)
        juce::FloatVectorOperations::add(c % 2 == 0 ? l : r, s.getReadPointer(c), numSamples);
    mixChannel(channel, l, r, numSamples);
}

void AudioEngine::apply(const SetDrumSound& command) noexcept
{
    moduleOf(command.source).voices.setDrumSound(command.index, command.sound);
}

void AudioEngine::apply(const SetSynthSound& command) noexcept
{
    moduleOf(command.source).voices.setSynthSound(command.index, command.sound);
}

void AudioEngine::apply(const TransitionFxSetEffect& command) noexcept
{
    const auto side = std::min<std::size_t>(command.side, 1);
    retireEffect(transitionFx[side]);
    transitionFx[side] = command.effect;
    transitionFxTail[side] = 0;
}

void AudioEngine::processTransitionFx(int numSamples) noexcept
{
    // The two send effects: what their FX lanes sent this block, then their tails until they have run out.
    for (std::size_t side = 0; side < 2; ++side)
    {
        auto* fx = transitionFx[side];
        if (fx == nullptr || (!transitionFxFed[side] && transitionFxTail[side] <= 0))
            continue;
        float* l = transitionFxBus.getWritePointer(static_cast<int>(2 * side));
        float* r = transitionFxBus.getWritePointer(static_cast<int>(2 * side + 1));
        fx->process(l, r, numSamples, beatContext());
        masterBus.addFrom(0, 0, l, numSamples);
        masterBus.addFrom(1, 0, r, numSamples);
        transitionFxTail[side] = transitionFxFed[side] ? static_cast<int>(16.0 * sampleRate) : transitionFxTail[side] - numSamples;
    }
}

void AudioEngine::apply(const SetSampleSound& command) noexcept
{
    moduleOf(command.source).voices.setSample(command.index, command.sample);
}

void AudioEngine::apply(const BeatMakerStop&) noexcept
{
    beatMaker->due.clear();
    EventQueue::value_type e{};
    while (beatMaker->queue.tryPop(e))
    {
    }
    beatMaker->voices.releaseAll();
}

void AudioEngine::apply(const CodeDeckStop&) noexcept
{
    codeDeck->due.clear();
    EventQueue::value_type e{};
    while (codeDeck->queue.tryPop(e))
    {
    }
    codeDeck->voices.releaseAll();
}

void AudioEngine::renderSampler(int numSamples) noexcept
{
    float* l = deckScratch.getWritePointer(0);
    float* r = deckScratch.getWritePointer(1);
    // Quantized presses whose beat comes in this block.
    if (samplerPending)
    {
        samplerPending = false;
        for (int s = 0; s < Sampler::kSlots; ++s)
        {
            auto& beat = samplerPendingBeat[static_cast<std::size_t>(s)];
            if (beat < 0.0)
                continue;
            const double offset = clock.sampleAt(beat) - static_cast<double>(clock.blockStart());
            if (offset < numSamples)
            {
                sampler.press(s, static_cast<int>(std::max(0.0, std::round(offset))));
                beat = -1.0;
            }
            else
            {
                samplerPending = true;
            }
        }
    }
    // The sequencer's presses due in this block (P3-15), at their samples.
    if (clock.beatsPerSample() > 0.0)
        sequencer.process(sampler, clock.beatAtBlockStart(), clock.beatAtBlockEnd(), 1.0 / clock.beatsPerSample(),
                          numSamples);
    renderSequencerClick(numSamples);
    juce::FloatVectorOperations::clear(l, numSamples);
    juce::FloatVectorOperations::clear(r, numSamples);
    sampler.render(l, r, numSamples, clock.bpm());
    mixChannel(kSamplerChannel, l, r, numSamples);
    if (releaseFx[kSamplerChannel].finished()) // a release FX ended the sampler: it stops
    {
        sampler.stopAll();
        releaseFx[kSamplerChannel].reset();
    }
}

void AudioEngine::mixChannel(int channel, float* l, float* r, int numSamples, float* dl, float* dr) noexcept
{
    const auto i = static_cast<std::size_t>(channel);
    auto& m = mixer[i];
    {
        strips[i].processPreFader(l, r, numSamples);
        colorFx[i].process(l, r, numSamples, beatContext());   // Color FX (P3-10)
        for (auto& pad : padFx[i])                              // Pad FX (P3-12)
            pad.process(l, r, numSamples, beatContext());
        releaseFx[i].process(l, r, numSamples, beatContext()); // Release FX (P3-11)
        processFx(channel < kMaxDecks              ? static_cast<FxTarget>(channel)
                  : channel == kBeatMakerChannel ? FxTarget::BeatMaker
                  : channel == kCodeDeckChannel  ? FxTarget::CodeDeck
                                                 : FxTarget::Sampler,
                  l, r, numSamples); // FX units on this channel (P3-08)
        if (dl != nullptr && channel < kMaxDecks) // a deck's stems past the effects (P6-04), joining here
        {
            dryStrips[i].processPreFader(dl, dr, numSamples);
            for (int s = 0; s < numSamples; ++s)
            {
                l[s] += dl[s];
                r[s] += dr[s];
            }
        }
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
        if (m.autoGain) // a transition's volume lane (P4-14), along the block
        {
            const float g0 = m.autoGainStart, step = (m.autoGainEnd - m.autoGainStart) / static_cast<float>(numSamples);
            for (int s = 0; s < numSamples; ++s)
            {
                const float g = g0 + step * static_cast<float>(s);
                l[s] *= g;
                r[s] *= g;
            }
        }
        if (m.transitionSend >= 0 && (m.sendStart > 0.0f || m.sendEnd > 0.0f)) // its FX lane (P4-18): into the send
        {
            const auto side = static_cast<std::size_t>(m.transitionSend);
            float* sl = transitionFxBus.getWritePointer(static_cast<int>(2 * side));
            float* sr = transitionFxBus.getWritePointer(static_cast<int>(2 * side + 1));
            const float g0 = m.sendStart, step = (m.sendEnd - m.sendStart) / static_cast<float>(numSamples);
            for (int s = 0; s < numSamples; ++s)
            {
                const float g = g0 + step * static_cast<float>(s);
                sl[s] += g * l[s];
                sr[s] += g * r[s];
            }
            transitionFxFed[side] = true;
        }
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
    current.codeLateEvents = codeDeck->late; // P5-12
    current.codeVoices = codeDeck->voices.activeSampleVoices() + codeDeck->voices.activeSynthVoices() +
                         codeDeck->voices.activeDrumVoices();
    current.beatLateEvents = beatMaker->late; // P4-10
    current.beatVoices = beatMaker->voices.activeSampleVoices() + beatMaker->voices.activeSynthVoices() + beatMaker->voices.activeDrumVoices();
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
            fs.manualBpm = static_cast<float>(fxManualBpm[static_cast<std::size_t>(u)]);
            current.colorFx = static_cast<std::uint8_t>(colorType);
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
        s.stemsLoaded = d.stems() != nullptr; // P6-04
        s.stemsId = slot.stemsId;
        s.stemMute = s.stemSolo = s.stemFx = 0;
        for (int st = 0; st < Deck::kStems; ++st)
        {
            s.stemLevel[static_cast<std::size_t>(st)] = d.stemLevel(st);
            const auto bit = static_cast<std::uint8_t>(1u << st);
            if (d.stemMuted(st))
                s.stemMute |= bit;
            if (d.stemSoloed(st))
                s.stemSolo |= bit;
            if (d.stemFx(st))
                s.stemFx |= bit;
        }
        s.stemsActive = d.readingStems();
        s.swapLoaded = d.swapLoaded(); // P6-05
        s.swapActive = d.swapMask();
        s.swapQueued = -1;
        for (int q = 0; q < slot.queuedCount; ++q)
            if (slot.queued[static_cast<std::size_t>(q)].kind == DeckSlot::QueuedAction::Kind::Swap)
                s.swapQueued = slot.queued[static_cast<std::size_t>(q)].swapMask;
        s.stemsDry = d.dryActive();
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
        {
            const auto stage = releaseFx[i].stage();
            s.releaseFx = stage == dsp::fx::ReleaseFx::Stage::Idle   ? 0
                          : stage == dsp::fx::ReleaseFx::Stage::Held ? 1
                                                                     : 2;
            s.releaseFxKind = static_cast<std::uint8_t>(releaseFx[i].kind());
            s.padFxOn = s.padFxActive = 0;
            for (int p = 0; p < kPadFxSlots; ++p)
            {
                const auto& pad = padFx[i][static_cast<std::size_t>(p)];
                if (pad.isOn())
                    s.padFxOn = static_cast<std::uint8_t>(s.padFxOn | (1u << p));
                if (pad.isActive())
                    s.padFxActive = static_cast<std::uint8_t>(s.padFxActive | (1u << p));
            }
        }
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
        c.filter = static_cast<float>(filterKnob[i]); // the knob (it drives the strip's filter or the color FX)
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
    for (int s = 0; s < Sampler::kSlots; ++s) // P3-13
    {
        const auto& slot = sampler.slot(s);
        auto& st = current.sampler[static_cast<std::size_t>(s)];
        st.loaded = slot.buffer != nullptr;
        st.playing = sampler.isPlaying(s) || samplerPendingBeat[static_cast<std::size_t>(s)] >= 0.0;
        st.mode = static_cast<std::uint8_t>(slot.mode);
        st.sync = slot.sync;
        st.gain = slot.gain;
        st.bpm = static_cast<float>(slot.bpm);
        st.progress = static_cast<float>(sampler.progress(s));
    }
    current.transition.active = transition.active; // P3-17
    current.transition.waiting = transition.active && !transition.started;
    current.transition.from = transition.from;
    current.transition.to = transition.to;
    current.transition.style = static_cast<std::uint8_t>(transition.style);
    current.transition.beats = static_cast<float>(transition.beats);
    current.transition.startBeat = transition.startBeat;
    current.transition.curves = transition.active && transition.curves != nullptr; // P4-14
    current.transition.lanesUsed = transition.lanesUsed;
    current.transition.lanes = transition.lanes;
    current.transition.progress = transition.active && transition.started
                                      ? static_cast<float>(std::clamp((clock.beatAtBlockEnd() - transition.startBeat) / transition.beats, 0.0, 1.0))
                                      : 0.0f;
    current.sequencer.playing = sequencer.playing(); // P3-15
    current.sequencer.recording = sequencer.recording();
    current.sequencer.metronome = sequencerMetronome;
    current.sequencer.mutes = sequencer.mutedSlots();
    current.sequencer.position = sequencer.positionAt(clock.beatAtBlockEnd());
    current.sequencer.sequence = sequencer.sequence();
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
