#include "engine/Deck.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace devildj::engine
{

namespace
{
constexpr float kHalfPi = static_cast<float>(std::numbers::pi / 2.0);
}

void Deck::prepare(double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    fadeSamples = std::max(1, static_cast<int>(std::lround(kFadeSeconds * sampleRate)));
    resampler.prepare();

    maxBlock = std::max(1, maxBlockSize);
    maxInput = 2 * maxBlock + 64; // fastest playback: +50 % tempo x +4 % bend
    if (stretcher == nullptr)
        stretcher = dsp::makeSignalsmithStretcher();
    stretcher->prepare(2, sampleRate);
    stretcher->setTransposeSemitones(keyShiftSemitones);
    if (dryStretcher == nullptr)
        dryStretcher = dsp::makeSignalsmithStretcher();
    dryStretcher->prepare(2, sampleRate);
    dryStretcher->setTransposeSemitones(keyShiftSemitones);
    dryPrimed = false;
    const auto seekLength = static_cast<std::size_t>(stretcher->seekLength());
    for (auto* v : {&stretchInL, &stretchInR, &dryInL, &dryInR})
        v->assign(static_cast<std::size_t>(maxInput), 0.0f);
    for (auto* v : {&stretchOutL, &stretchOutR, &dryOutL, &dryOutR})
        v->assign(static_cast<std::size_t>(maxBlock), 0.0f);
    for (auto* v : {&seekL, &seekR})
        v->assign(seekLength, 0.0f);
    blockRates.assign(static_cast<std::size_t>(maxBlock), 1.0);
    keepRamp.assign(static_cast<std::size_t>(maxBlock), 1.0f); // Drum Swap (P6-05)
    for (auto& r : swapRamps)
        r.assign(static_cast<std::size_t>(maxBlock), 0.0f);
    stretcherPrimed = false;
}

// ---- jog / scratch ----

void Deck::scratchBegin(double timeSeconds) noexcept
{
    if (buffer == nullptr || scratch == Scratch::Holding)
        return;
    if (stopKind != StopKind::None) // the hand catches a braking record
    {
        stopKind = StopKind::None;
        if ((slipReasons & kSlipPause) != 0) // the stop's slip pause becomes the scratch's
            slipReasons = static_cast<std::uint8_t>((slipReasons & ~kSlipPause) | kSlipScratch);
    }
    const bool sounding = state != Transport::Paused || scratch == Scratch::Releasing;
    if (state == Transport::Playing && scratch == Scratch::None)
        beginSlip(kSlipScratch);
    if (!sounding)
        startSounding(); // scrub from pause: fade in
    else if (keyLockOn && scratch == Scratch::None)
    {
        spawnTail(playhead, currentMainGain()); // leave the stretcher: crossfade to the direct read
        mainFadeRemaining = fadeSamples;
    }
    scratchRate = sounding ? lastRate : 0.0;
    scratchDesired = 0.0; // hand down = the record stops under it until it moves
    scratchTarget = playhead;
    samplesSinceMove = 0.0;
    moveInterval = 0.0167 * sampleRate;
    moveCount = 0;
    lastMoveTime = timeSeconds;
    atEnd = false;
    scratch = Scratch::Holding;
}

void Deck::scratchMove(double deltaSamples, double timeSeconds) noexcept
{
    if (scratch != Scratch::Holding)
        return;
    const double before = scratchTarget;
    scratchTarget = std::clamp(scratchTarget + deltaSamples, 0.0, static_cast<double>(length()));
    const bool timed = timeSeconds >= 0.0 && lastMoveTime >= 0.0;
    const double measured = timed ? (timeSeconds - lastMoveTime) * sampleRate : samplesSinceMove;
    const double interval = std::clamp(measured, 0.004 * sampleRate, 0.05 * sampleRate);
    lastMoveTime = timeSeconds;
    moveInterval = 0.7 * moveInterval + 0.3 * interval;
    samplesSinceMove = 0.0;

    moveDeltas[static_cast<std::size_t>(moveIndex)] = scratchTarget - before;
    moveIntervals[static_cast<std::size_t>(moveIndex)] = interval;
    moveIndex = (moveIndex + 1) % kMoveWindow;
    moveCount = std::min(moveCount + 1, kMoveWindow);
    double distance = 0.0, time = 0.0;
    for (int i = 0; i < moveCount; ++i)
    {
        distance += moveDeltas[static_cast<std::size_t>(i)];
        time += moveIntervals[static_cast<std::size_t>(i)];
    }
    const double velocity = distance / time;
    // Aim one move interval behind the hand (where a steady hand puts the playhead at each event); correct the
    // remaining error gently over 100 ms, held until the next move so there's no ripple between events.
    const double error = scratchTarget - playhead - velocity * moveInterval;
    scratchDesired = velocity + error / (0.1 * sampleRate);
}

void Deck::scratchEnd() noexcept
{
    if (scratch != Scratch::Holding)
        return;
    if ((slipReasons & kSlipScratch) != 0)
    {
        slipReasons = static_cast<std::uint8_t>(slipReasons & ~kSlipScratch);
        if (slipReasons == 0 && state != Transport::Paused) // slip: back to where the track would be
        {
            scratch = Scratch::None;
            slipReturn(); // the jump's crossfade also takes key lock back to the stretcher
            return;
        }
    }
    if (state != Transport::Paused) // playing: ease back to the normal rate
    {
        scratch = Scratch::Releasing;
        releaseRemaining = std::max(1, static_cast<int>(std::lround(kScratchReleaseSeconds * sampleRate)));
        releaseFromRate = scratchRate;
    }
    else // scrubbing: stay paused here
    {
        scratch = Scratch::None;
        stopSounding();
    }
}

void Deck::nudge(double deltaSamples) noexcept
{
    if (buffer == nullptr)
        return;
    if (state == Transport::Paused && scratch == Scratch::None)
        playhead = std::clamp(playhead + deltaSamples, 0.0, static_cast<double>(length())); // silent fine move
    else
        nudgeRemaining += deltaSamples;
}

double Deck::nextScratchRate(double playheadNow, double normalRate) noexcept
{
    if (scratch == Scratch::Holding)
    {
        samplesSinceMove += 1.0;
        // No new move for a while: the hand has stopped; settle onto it.
        if (samplesSinceMove > 1.5 * moveInterval)
        {
            scratchDesired = (scratchTarget - playheadNow) / (0.02 * sampleRate);
            moveCount = 0; // next move starts a fresh speed estimate
        }
        const double desired = std::clamp(scratchDesired, -kMaxScratchRate, kMaxScratchRate);
        const double alpha = 1.0 - std::exp(-1.0 / (0.005 * sampleRate)); // 5 ms smoother
        scratchRate += alpha * (desired - scratchRate);
        return scratchRate;
    }
    // Releasing: linear ramp to the normal rate.
    const double t = 1.0 - static_cast<double>(releaseRemaining) /
                               std::max(1.0, std::lround(kScratchReleaseSeconds * sampleRate) * 1.0);
    --releaseRemaining;
    return releaseFromRate + (normalRate - releaseFromRate) * t;
}

void Deck::finishScratchRelease() noexcept
{
    scratch = Scratch::None;
    if (keyLockOn && state != Transport::Paused) // back to the stretcher with a crossfade
    {
        spawnTail(playhead, currentMainGain());
        mainFadeRemaining = fadeSamples;
        stretcherPrimed = false;
    }
}

void Deck::setKeyLock(bool on) noexcept
{
    if (on == keyLockOn)
        return;
    if (state != Transport::Paused) // switch sound sources with the usual 2 ms crossfade
    {
        spawnTail(playhead, currentMainGain());
        mainFadeRemaining = fadeSamples;
    }
    keyLockOn = on;
    stretcherPrimed = false;
}

void Deck::setKeyShift(int semitones) noexcept
{
    keyShiftSemitones = std::clamp(semitones, -12, 12);
    if (stretcher != nullptr)
        stretcher->setTransposeSemitones(keyShiftSemitones);
    if (dryStretcher != nullptr)
        dryStretcher->setTransposeSemitones(keyShiftSemitones);
}

// ---- tempo ----

void Deck::setTempoPercent(double percent) noexcept
{
    tempo = std::clamp(percent, -range, range);
}

void Deck::nudgeTempo(double deltaPercent) noexcept
{
    setTempoPercent(tempo + deltaPercent);
}

void Deck::setSyncTempo(double percent) noexcept
{
    for (const double r : {6.0, 10.0, 16.0, 50.0})
        if (std::abs(percent) <= r || r == 50.0)
        {
            if (r > range)
                range = r;
            break;
        }
    setTempoPercent(percent);
}

void Deck::setRateCorrection(double factor) noexcept
{
    correction = std::clamp(factor, 1.0 - kMaxRateCorrection, 1.0 + kMaxRateCorrection);
}

void Deck::setTempoRange(double rangePercent) noexcept
{
    constexpr double ranges[] = {6.0, 10.0, 16.0, 50.0};
    double best = ranges[0];
    for (double r : ranges)
        if (std::abs(r - rangePercent) < std::abs(best - rangePercent))
            best = r;
    range = best;
    setTempoPercent(tempo);
}

void Deck::setPitchBend(double percent) noexcept
{
    bendTarget = std::clamp(percent, -kMaxPitchBendPercent, kMaxPitchBendPercent);
}

void Deck::setTrack(const TrackBuffer* track) noexcept
{
    buffer = track;
    stemBuffer = nullptr; // the old track's (the engine hands them back)
    resetStems();
    swapOn = 0; // the slots stay; the new track starts with its own drums
    keepGain = 1.0f;
    for (auto& s : swaps)
        s.gain = 0.0f;
    state = Transport::Paused;
    playhead = 0.0;
    cue = 0.0;
    atEnd = false;
    cueHeldAfterBackCue = false;
    mainFadeRemaining = 0;
    tails = {};
    stretcherPrimed = false;
    scratch = Scratch::None;
    nudgeRemaining = 0.0;
    loopOn = false;
    loopIn = loopOut = 0.0;
    slipReasons = 0;
    reverseOn = false;
    stopKind = StopKind::None;
}

bool Deck::isSilent() const noexcept
{
    if (state != Transport::Paused || scratch != Scratch::None)
        return false;
    return std::none_of(tails.begin(), tails.end(), [](const Tail& t) { return t.active; });
}

// ---- transport ----

void Deck::play() noexcept
{
    if (buffer == nullptr)
        return;
    if (state == Transport::CuePreview)
    {
        state = Transport::Playing; // keep going; the cue release won't pull us back
        return;
    }
    if (state == Transport::Playing)
    {
        if (stopKind != StopKind::None)
            cancelStop(); // play during a brake / spin-back: back to the normal rate
        return;
    }
    if ((slipReasons & kSlipPause) != 0) // slip pause: on from where the track would be now
    {
        slipReasons = static_cast<std::uint8_t>(slipReasons & ~kSlipPause);
        if (slipReasons == 0)
            slipReturn();
    }
    if (atEnd)
        return; // nothing left to play; seek or cue first
    state = Transport::Playing;
    startSounding();
}

void Deck::pause() noexcept
{
    if (state == Transport::Paused)
        return;
    if (state == Transport::Playing)
        beginSlip(kSlipPause);
    stopKind = StopKind::None;
    state = Transport::Paused;
    stopSounding();
}

void Deck::togglePlay() noexcept
{
    if (state == Transport::Playing)
        pause();
    else
        play();
}

void Deck::cuePress() noexcept
{
    if (buffer == nullptr)
        return;

    switch (state)
    {
        case Transport::Playing: // back cue: return to the cue point and pause
            slipReasons = 0;
            stopKind = StopKind::None;
            state = Transport::Paused;
            stopSounding();
            playhead = cue;
            atEnd = false;
            cueHeldAfterBackCue = true;
            break;

        case Transport::Paused: // set the cue here, preview while held
            cue = playhead;
            cueHeldAfterBackCue = false;
            if (!atEnd)
            {
                state = Transport::CuePreview;
                startSounding();
            }
            break;

        case Transport::CuePreview:
            break; // already held
    }
}

void Deck::cueRelease() noexcept
{
    if (cueHeldAfterBackCue)
    {
        cueHeldAfterBackCue = false;
        return;
    }
    if (state != Transport::CuePreview)
        return;
    state = Transport::Paused;
    stopSounding();
    playhead = cue;
    atEnd = false;
}

void Deck::setCuePoint(double samplePosition) noexcept
{
    cue = std::clamp(samplePosition, 0.0, static_cast<double>(length()));
}

void Deck::setLoop(double start, double end) noexcept
{
    if (buffer == nullptr)
        return;
    const double len = length();
    start = std::clamp(start, 0.0, len);
    end = std::clamp(end, 0.0, len);
    if (end - start < 1.0)
        return;
    if (state == Transport::Playing)
        beginSlip(kSlipLoop); // the shadow starts where the playhead is now (before any jump into the loop)
    loopIn = start;
    loopOut = end;
    loopOn = true;
}

void Deck::exitLoop() noexcept
{
    loopOn = false;
    endSlip(kSlipLoop);
}

void Deck::setSlip(bool on) noexcept
{
    slipEnabled = on;
    if (!on)
        slipReasons = 0; // slip off while slipping: stay where you are
}

void Deck::beginSlip(std::uint8_t reason) noexcept
{
    if (!slipEnabled || buffer == nullptr)
        return;
    if (slipReasons == 0)
    {
        if (state != Transport::Playing)
            return;
        shadow = playhead;
    }
    slipReasons = static_cast<std::uint8_t>(slipReasons | reason);
}

void Deck::beginSlicerHold() noexcept
{
    if (buffer == nullptr)
        return;
    if (slipReasons == 0)
    {
        if (state != Transport::Playing)
            return;
        shadow = playhead;
    }
    slipReasons = static_cast<std::uint8_t>(slipReasons | kSlipSlice);
}

void Deck::endSlip(std::uint8_t reason) noexcept
{
    if ((slipReasons & reason) == 0)
        return;
    slipReasons = static_cast<std::uint8_t>(slipReasons & ~reason);
    if (slipReasons == 0)
        slipReturn();
}

void Deck::slipReturn() noexcept
{
    const double target = std::min(shadow, static_cast<double>(length()));
    if (loopOn && (target < loopIn || target >= loopOut))
        loopOn = false; // a loop set while not slipping doesn't pull the shadow back into it
    if (state != Transport::Paused)
    {
        jumpTo(target);
    }
    else
    {
        playhead = target;
        atEnd = target >= length();
        stretcherPrimed = false;
    }
}

void Deck::wrapLoop() noexcept
{
    const double length = loopOut - loopIn;
    if (length <= 0.0)
        return;
    const double over = std::fmod(std::max(0.0, playhead - loopOut), length);
    jumpTo(loopIn + over); // the old head fades out past the end while the new one fades in from the start
}

// ---- reverse, brake, spin-back (P3-05) ----

bool Deck::stretchedNow() const noexcept
{
    return keyLockOn && state != Transport::Paused && scratch == Scratch::None && !reverseOn &&
           stopKind == StopKind::None && stretcher != nullptr;
}

void Deck::switchSource(bool wasStretched) noexcept
{
    if (state == Transport::Paused || wasStretched == stretchedNow())
        return;
    spawnTail(playhead, currentMainGain());
    mainFadeRemaining = fadeSamples;
    stretcherPrimed = false;
}

void Deck::setReverse(bool on) noexcept
{
    if (on == reverseOn || buffer == nullptr)
        return;
    const bool was = stretchedNow();
    if (on)
        beginSlip(kSlipReverse);
    reverseOn = on;
    switchSource(was);
    if (!on)
        endSlip(kSlipReverse); // slip: back to where the track would be
}

void Deck::startStop(StopKind kind, double seconds) noexcept
{
    if (buffer == nullptr || state != Transport::Playing || scratch != Scratch::None || kind == StopKind::None)
        return;
    const bool was = stretchedNow();
    beginSlip(kSlipPause); // slip: the track goes on from the first sample of the stop
    stopFrom = (reverseOn ? -1.0 : 1.0) * rate();
    stopKind = kind;
    stopTotal = std::max(1, static_cast<int>(std::lround(std::clamp(seconds, 0.01, 30.0) * sampleRate)));
    stopElapsed = 0;
    nudgeRemaining = 0.0;
    switchSource(was);
}

void Deck::cancelStop() noexcept
{
    const bool was = stretchedNow();
    stopKind = StopKind::None;
    switchSource(was);
    endSlip(kSlipPause);
}

double Deck::nextStopRate() noexcept
{
    const double elapsed = static_cast<double>(++stopElapsed);
    const double total = static_cast<double>(stopTotal);
    if (elapsed >= total)
        return 0.0;
    if (stopKind == StopKind::Brake)
        return stopFrom * (1.0 - elapsed / total);
    // Spin-back: a quick push backwards, then it slows (quadratically) to a stop.
    const double spin = -stopFrom * kSpinBackRate;
    const double attack = std::min(kSpinAttackSeconds * sampleRate, 0.3 * total);
    if (elapsed <= attack)
        return stopFrom + (spin - stopFrom) * (elapsed / attack);
    const double v = 1.0 - (elapsed - attack) / (total - attack);
    return spin * v * v;
}

void Deck::wrapLoopBack() noexcept
{
    const double length = loopOut - loopIn;
    if (length <= 0.0)
        return;
    const double under = std::fmod(std::max(0.0, loopIn - playhead), length);
    jumpTo(loopOut - under);
}

void Deck::seek(double samplePosition) noexcept
{
    if (buffer == nullptr)
        return;
    // Out of a playing loop: the loop ends (P3-03).
    if (loopOn && (samplePosition < loopIn || samplePosition >= loopOut))
    {
        loopOn = false;
        slipReasons = static_cast<std::uint8_t>(slipReasons & ~kSlipLoop);
    }
    slipReasons = static_cast<std::uint8_t>(slipReasons & ~kSlipPause); // a seek while paused: play from there
    jumpTo(std::clamp(samplePosition, 0.0, static_cast<double>(length())));
}

// ---- declicking ----

void Deck::spawnTail(double position, float gain) noexcept
{
    if (gain <= 0.0f)
        return;
    // Use a free slot, or replace the quietest tail.
    Tail* slot = &tails[0];
    for (auto& t : tails)
    {
        if (!t.active)
        {
            slot = &t;
            break;
        }
        if (t.startGain < slot->startGain)
            slot = &t;
    }
    *slot = Tail{true, position, fadeSamples, gain};
}

float Deck::currentMainGain() const noexcept
{
    if (mainFadeRemaining <= 0)
        return 1.0f;
    const float t = 1.0f - static_cast<float>(mainFadeRemaining) / static_cast<float>(fadeSamples);
    return std::sin(kHalfPi * t);
}

void Deck::startSounding() noexcept
{
    mainFadeRemaining = fadeSamples;
    stretcherPrimed = false;
}

void Deck::stopSounding() noexcept
{
    spawnTail(playhead, currentMainGain());
    mainFadeRemaining = 0;
}

void Deck::jumpTo(double samplePosition) noexcept
{
    atEnd = samplePosition >= length();
    if (state != Transport::Paused)
    {
        spawnTail(playhead, currentMainGain());
        mainFadeRemaining = fadeSamples;
    }
    playhead = samplePosition;
    stretcherPrimed = false;
    if (atEnd && state != Transport::Paused)
    {
        state = Transport::Paused; // jumped to the very end: nothing to play
        mainFadeRemaining = 0;
    }
}

// ---- rendering ----

float Deck::readSample(int channel, double position, double rate) const noexcept
{
    return resampler.read(buffer->audio.getReadPointer(channel), length(), position, rate);
}

void Deck::readFrames(std::int64_t from, int count, float* left, float* right) const noexcept
{
    const auto len = static_cast<std::int64_t>(length());
    const float* l = buffer->audio.getReadPointer(0);
    const float* r = buffer->audio.getReadPointer(1);
    for (int i = 0; i < count; ++i)
    {
        const auto p = from + i;
        const bool inside = p >= 0 && p < len;
        left[i] = inside ? l[p] : 0.0f;
        right[i] = inside ? r[p] : 0.0f;
    }
}

// ---- stems (P6-04) ----

void Deck::setStems(const StemBuffer* stems) noexcept
{
    if (stemBuffer == nullptr) // from the track to its stems: start where the track is (every stem at 1)
    {
        mainGain.fill(1.0f);
        dryGain.fill(0.0f);
    }
    stemBuffer = stems;
    updateStemTargets();
}

void Deck::setStemLevel(int stem, float level) noexcept
{
    if (stem < 0 || stem >= kStems)
        return;
    stemSettings[static_cast<std::size_t>(stem)].level = std::clamp(level, 0.0f, 1.0f);
    updateStemTargets();
}

void Deck::setStemMute(int stem, bool on) noexcept
{
    if (stem < 0 || stem >= kStems)
        return;
    stemSettings[static_cast<std::size_t>(stem)].mute = on;
    updateStemTargets();
}

void Deck::setStemSolo(int stem, bool on) noexcept
{
    if (stem < 0 || stem >= kStems)
        return;
    stemSettings[static_cast<std::size_t>(stem)].solo = on;
    updateStemTargets();
}

void Deck::setStemFx(int stem, bool on) noexcept
{
    if (stem < 0 || stem >= kStems)
        return;
    stemSettings[static_cast<std::size_t>(stem)].fx = on;
    updateStemTargets();
}

void Deck::resetStems() noexcept
{
    stemSettings.fill(StemSetting{});
    mainGain.fill(1.0f);
    dryGain.fill(0.0f);
    updateStemTargets();
}

// ---- Drum Swap (P6-05) ----

void Deck::setSwapLoop(int slot, const TrackBuffer* loop, double beats) noexcept
{
    if (slot < 0 || slot >= kSwapSlots)
        return;
    auto& s = swaps[static_cast<std::size_t>(slot)];
    s.loop = loop != nullptr && loop->numSamples() > 0 && beats > 0.0 ? loop : nullptr;
    s.beats = s.loop != nullptr ? beats : 0.0;
    if (s.loop == nullptr)
    {
        s.gain = 0.0f;
        swapOn = static_cast<std::uint8_t>(swapOn & ~(1u << slot));
    }
}

void Deck::setSwap(std::uint8_t mask, double anchor, double deckSamplesPerBeat) noexcept
{
    for (int k = 0; k < kSwapSlots; ++k)
    {
        const auto bit = static_cast<std::uint8_t>(1u << k);
        auto& s = swaps[static_cast<std::size_t>(k)];
        if (s.loop == nullptr)
            mask = static_cast<std::uint8_t>(mask & ~bit);
        else if ((mask & bit) && !(swapOn & bit)) // starting: from its first beat at the anchor
        {
            s.anchor = anchor;
            s.deckSamplesPerBeat = deckSamplesPerBeat > 0.0 ? deckSamplesPerBeat : sampleRate / 2.0;
        }
    }
    swapOn = mask;
}

double Deck::readAhead() const noexcept
{
    if (!stretchedNow() || stretcher == nullptr)
        return 0.0;
    return static_cast<double>(stretcher->inputLatency()) + stretcher->outputLatency() * rate();
}

std::uint8_t Deck::swapLoaded() const noexcept
{
    std::uint8_t m = 0;
    for (int k = 0; k < kSwapSlots; ++k)
        if (swaps[static_cast<std::size_t>(k)].loop != nullptr)
            m = static_cast<std::uint8_t>(m | (1u << k));
    return m;
}

// A loop at a deck position: the deck's beat since the anchor, wrapped to the loop, 4-point Hermite with the
// loop's ends joined (a seamless loop).
float Deck::swapRead(int slot, int channel, double position) const noexcept
{
    const auto& s = swaps[static_cast<std::size_t>(slot)];
    const int len = s.loop->numSamples();
    const double beat = (position - s.anchor) / s.deckSamplesPerBeat;
    double b = std::fmod(beat, s.beats);
    if (b < 0.0)
        b += s.beats;
    const double p = b * (static_cast<double>(len) / s.beats);
    const auto i = static_cast<int>(std::floor(p));
    const auto t = static_cast<float>(p - i);
    const float* d = s.loop->audio.getReadPointer(std::min(channel, s.loop->audio.getNumChannels() - 1));
    const auto at = [d, len](int k) { return d[((k % len) + len) % len]; };
    const float xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

// The drums stem's place in the mix: the stem itself and / or the swap loops, by this part's ramps.
float Deck::drumsAt(int channel, double position, double rate, int index) const noexcept
{
    const auto i = static_cast<std::size_t>(std::clamp(index, 0, partSamples - 1));
    float v = 0.0f;
    if (keepRamp[i] != 0.0f)
        v += keepRamp[i] * resampler.read(stemBuffer->channel(0, channel), stemBuffer->length, position, rate);
    if (swapPart)
        for (int k = 0; k < kSwapSlots; ++k)
        {
            const float g = swapRamps[static_cast<std::size_t>(k)][i];
            if (g != 0.0f)
                v += g * swapRead(k, channel, position);
        }
    return v;
}

void Deck::updateStemTargets() noexcept
{
    bool anySolo = false;
    for (const auto& s : stemSettings)
        anySolo = anySolo || s.solo;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kStems); ++i)
    {
        const auto& s = stemSettings[i];
        const bool on = anySolo ? s.solo : !s.mute;
        const float g = on ? s.level : 0.0f;
        mainTarget[i] = s.fx ? g : 0.0f;
        dryTarget[i] = s.fx ? 0.0f : g;
    }
}

// This part's gain ramps (at most a full swing per kStemRampSeconds), and whether it reads the stems at all.
void Deck::beginStemPart(int numSamples) noexcept
{
    const auto step = static_cast<float>(numSamples / std::max(1.0, kStemRampSeconds * sampleRate));
    const auto toward = [step](float from, float target)
    { return from < target ? std::min(target, from + step) : std::max(target, from - step); };
    bool defaults = true;
    bool dry = false;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kStems); ++i)
    {
        mainFrom[i] = mainGain[i];
        dryFrom[i] = dryGain[i];
        mainGain[i] = mainTo[i] = toward(mainGain[i], mainTarget[i]);
        dryGain[i] = dryTo[i] = toward(dryGain[i], dryTarget[i]);
        defaults = defaults && mainFrom[i] == 1.0f && mainTo[i] == 1.0f && dryFrom[i] == 0.0f && dryTo[i] == 0.0f;
        dry = dry || dryFrom[i] > 0.0f || dryTo[i] > 0.0f;
    }
    // Drum Swap: the drums stem's share and each loop's, sample by sample (a 4 ms crossfade on the sample).
    partSamples = std::max(1, numSamples);
    const auto swapStep = static_cast<float>(1.0 / std::max(1.0, kSwapFadeSeconds * sampleRate));
    swapPart = false;
    bool keepMoves = false;
    const float keepTarget = swapOn != 0 && stemBuffer != nullptr ? 0.0f : 1.0f;
    for (int n = 0; n < numSamples; ++n)
    {
        keepGain = keepGain < keepTarget ? std::min(keepTarget, keepGain + swapStep)
                                         : std::max(keepTarget, keepGain - swapStep);
        keepRamp[static_cast<std::size_t>(n)] = keepGain;
        keepMoves = keepMoves || keepGain != 1.0f;
    }
    for (int k = 0; k < kSwapSlots; ++k)
    {
        auto& s = swaps[static_cast<std::size_t>(k)];
        auto& ramp = swapRamps[static_cast<std::size_t>(k)];
        const float target = (swapOn & (1u << k)) && s.loop != nullptr && stemBuffer != nullptr ? 1.0f : 0.0f;
        if (s.loop == nullptr)
            s.gain = 0.0f;
        bool any = false;
        for (int n = 0; n < numSamples; ++n)
        {
            s.gain = s.gain < target ? std::min(target, s.gain + swapStep) : std::max(target, s.gain - swapStep);
            ramp[static_cast<std::size_t>(n)] = s.gain;
            any = any || s.gain != 0.0f;
        }
        swapPart = swapPart || any;
    }
    lastSwapSounding = lastSwapSounding || swapPart;
    useStems = stemBuffer != nullptr && (!defaults || swapPart || keepMoves);
    useDry = useStems && dry;
    if (!useDry)
        dryPrimed = false;
}

// The stems at one position, each through the resampler with its gain (ramped: t = 0 .. 1 along the part).
float Deck::readMix(int channel, double position, double rate, const float* from, const float* to, float t,
                     int index) const noexcept
{
    float v = 0.0f;
    const int len = stemBuffer->length;
    for (int s = 0; s < kStems; ++s)
    {
        const float g = from[s] + (to[s] - from[s]) * t;
        if (g == 0.0f)
            continue;
        v += g * (s == 0 ? drumsAt(channel, position, rate, index) // the drums stem, or what swaps it (P6-05)
                         : resampler.read(stemBuffer->channel(s, channel), len, position, rate));
    }
    return v;
}

void Deck::readFramesMix(std::int64_t from, int count, float* left, float* right, const float* gFrom,
                         const float* gTo) const noexcept
{
    const auto len = static_cast<std::int64_t>(stemBuffer->length);
    constexpr float kScale = 1.0f / 32768.0f;
    for (int i = 0; i < count; ++i)
    {
        const auto p = from + i;
        const float t = count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 1.0f;
        const int index = static_cast<int>(static_cast<std::int64_t>(i) * partSamples / std::max(1, count));
        float l = 0.0f, r = 0.0f;
        if (p >= 0 && p < len)
            for (int s = 0; s < kStems; ++s)
            {
                const float g = gFrom[s] + (gTo[s] - gFrom[s]) * t;
                if (g == 0.0f)
                    continue;
                if (s == 0 && (swapPart || keepRamp[static_cast<std::size_t>(std::min(index, partSamples - 1))] != 1.0f))
                {
                    l += g * drumsAt(0, static_cast<double>(p), 1.0, index); // with Drum Swap (P6-05)
                    r += g * drumsAt(1, static_cast<double>(p), 1.0, index);
                    continue;
                }
                l += g * kScale * static_cast<float>(stemBuffer->channel(s, 0)[p]);
                r += g * kScale * static_cast<float>(stemBuffer->channel(s, 1)[p]);
            }
        left[i] = l;
        right[i] = r;
    }
}

// Heard position = input fed - inputLatency - outputLatency x rate. Prime so the next output is the playhead.
void Deck::primeStretcher(double rate) noexcept
{
    const auto lead = static_cast<double>(stretcher->inputLatency()) + stretcher->outputLatency() * rate;
    const auto end = static_cast<std::int64_t>(std::llround(playhead + lead));
    const int n = stretcher->seekLength();
    if (useStems)
        readFramesMix(end - n, n, seekL.data(), seekR.data(), mainFrom.data(), mainFrom.data());
    else
        readFrames(end - n, n, seekL.data(), seekR.data());
    const float* in[] = {seekL.data(), seekR.data()};
    stretcher->reset();
    stretcher->seek(in, n, rate);
    feedPosition = end;
    stretcherPrimed = true;
    dryPrimed = false; // the dry stretcher primes at the same point when it's needed
}

void Deck::renderStretched(int numSamples, double playheadEnd, double rateEnd) noexcept
{
    const auto lead = static_cast<double>(stretcher->inputLatency()) + stretcher->outputLatency() * rateEnd;
    const auto target = static_cast<std::int64_t>(std::llround(playheadEnd + lead));
    const int count = static_cast<int>(std::clamp<std::int64_t>(target - feedPosition, 0, maxInput));
    if (useDry && !dryPrimed) // the dry output's stretcher, primed with the same history as the main one
    {
        const int n = dryStretcher->seekLength();
        readFramesMix(feedPosition - n, n, seekL.data(), seekR.data(), dryFrom.data(), dryFrom.data());
        const float* in[] = {seekL.data(), seekR.data()};
        dryStretcher->reset();
        dryStretcher->seek(in, n, blockRates[0]);
        dryPrimed = true;
    }
    if (useStems)
        readFramesMix(feedPosition, count, stretchInL.data(), stretchInR.data(), mainFrom.data(), mainTo.data());
    else
        readFrames(feedPosition, count, stretchInL.data(), stretchInR.data());
    if (useDry)
    {
        readFramesMix(feedPosition, count, dryInL.data(), dryInR.data(), dryFrom.data(), dryTo.data());
        const float* in[] = {dryInL.data(), dryInR.data()};
        float* out[] = {dryOutL.data(), dryOutR.data()};
        dryStretcher->process(in, count, out, numSamples);
    }
    feedPosition += count;

    const float* in[] = {stretchInL.data(), stretchInR.data()};
    float* out[] = {stretchOutL.data(), stretchOutR.data()};
    stretcher->process(in, count, out, numSamples);
}

void Deck::render(float* left, float* right, float* dryLeft, float* dryRight, int numSamples) noexcept
{
    std::fill(left, left + numSamples, 0.0f);
    std::fill(right, right + numSamples, 0.0f);
    if (dryLeft != nullptr)
    {
        std::fill(dryLeft, dryLeft + numSamples, 0.0f);
        std::fill(dryRight, dryRight + numSamples, 0.0f);
    }
    lastReadStems = lastDryActive = lastSwapSounding = false;
    if (buffer == nullptr)
        return;

    if (numSamples > maxBlock) // larger than prepared: split (keeps the scratch buffers big enough)
    {
        bool stems = false, dry = false, swap = false;
        for (int done = 0; done < numSamples; done += maxBlock)
        {
            const int n = std::min(maxBlock, numSamples - done);
            render(left + done, right + done, dryLeft != nullptr ? dryLeft + done : nullptr,
                   dryRight != nullptr ? dryRight + done : nullptr, n);
            stems = stems || lastReadStems;
            dry = dry || lastDryActive;
            swap = swap || lastSwapSounding;
        }
        lastReadStems = stems;
        lastDryActive = dry;
        lastSwapSounding = swap;
        return;
    }
    const auto at = [](float* p, int offset) { return p != nullptr ? p + offset : nullptr; };

    // A loop (P3-03): split the block at the sample the playhead reaches its end, wrap, go on.
    int done = 0;
    while (loopOn && state != Transport::Paused && scratch == Scratch::None && stopKind == StopKind::None &&
           done < numSamples)
    {
        if (playhead >= loopOut)
        {
            wrapLoop();
            continue;
        }
        if (reverseOn && playhead < loopIn) // reverse: went below the start at the end of the last part
        {
            wrapLoopBack();
            continue;
        }
        const double r = rate();
        if (r <= 0.0)
            break;
        // Forward: wraps once at or past the end. Reverse (P3-05): once below the start, back to the end.
        const double steps = reverseOn ? std::floor((playhead - loopIn) / r) + 1.0
                                       : std::ceil((loopOut - playhead) / r - 1e-9);
        if (steps >= static_cast<double>(numSamples - done))
            break;
        const int n = std::max(1, static_cast<int>(steps));
        renderPart(left + done, right + done, at(dryLeft, done), at(dryRight, done), n);
        done += n;
        if (!reverseOn && playhead >= loopOut)
            wrapLoop();
        else if (reverseOn && playhead < loopIn)
            wrapLoopBack();
    }
    if (done < numSamples)
        renderPart(left + done, right + done, at(dryLeft, done), at(dryRight, done), numSamples - done);
    // A brake / spin-back in a loop keeps it (to the block; the rate is changing anyway).
    if (loopOn && stopKind != StopKind::None && state != Transport::Paused)
    {
        if (playhead >= loopOut)
            wrapLoop();
        else if (playhead < loopIn)
            wrapLoopBack();
    }
}

void Deck::renderPart(float* left, float* right, float* dryLeft, float* dryRight, int numSamples) noexcept
{
    const double len = length();
    beginStemPart(numSamples);
    lastReadStems = lastReadStems || useStems;
    lastDryActive = lastDryActive || (useDry && dryLeft != nullptr);
    const float invN = 1.0f / static_cast<float>(numSamples);
    const double bendStep = kPitchBendSlewPercentPerSecond / sampleRate;

    // Rates for this block (bend slews, scratch follows the hand, nudges add an offset), so key lock knows
    // how far the playhead will move.
    double playheadEnd = playhead;
    double shadowEnd = shadow; // slip (P3-04): the normal rate, whatever the playhead does
    bool releaseFinished = false;
    bool stopFinished = false;
    for (int i = 0; i < numSamples; ++i)
    {
        if (bend != bendTarget)
            bend = bend < bendTarget ? std::min(bendTarget, bend + bendStep) : std::max(bendTarget, bend - bendStep);
        double r = this->rate();
        shadowEnd += r;
        if (scratch != Scratch::None && !releaseFinished)
        {
            r = nextScratchRate(playheadEnd, reverseOn ? -r : r); // a release eases back to the deck's way
            if (scratch == Scratch::Releasing && releaseRemaining <= 0)
                releaseFinished = true;
        }
        else if (stopKind != StopKind::None && state == Transport::Playing)
        {
            r = stopFinished ? 0.0 : nextStopRate();
            if (stopElapsed >= stopTotal)
                stopFinished = true;
        }
        else if (reverseOn && state != Transport::Paused)
        {
            r = -r;
        }
        if (scratch == Scratch::None && stopKind == StopKind::None && nudgeRemaining != 0.0)
        {
            const double offset = std::clamp(nudgeRemaining, -kMaxNudgeRateOffset, kMaxNudgeRateOffset);
            nudgeRemaining -= offset;
            r += offset;
        }
        blockRates[static_cast<std::size_t>(i)] = r;
        playheadEnd += r;
    }
    if (slipReasons != 0)
        shadow = std::min(shadowEnd, len);

    const bool sounding = state != Transport::Paused || scratch != Scratch::None;
    const bool stretched = stretchedNow() && !releaseFinished;
    if (stretched)
    {
        if (!stretcherPrimed)
            primeStretcher(blockRates[0]);
        renderStretched(numSamples, playheadEnd, blockRates[static_cast<std::size_t>(numSamples - 1)]);
    }

    for (int i = 0; i < numSamples; ++i)
    {
        float l = 0.0f, r = 0.0f, dl = 0.0f, dr = 0.0f;
        const double rate = blockRates[static_cast<std::size_t>(i)];
        const float t = static_cast<float>(i + 1) * invN; // along the stem gain ramps

        if (sounding)
        {
            const float g = currentMainGain();
            if (stretched)
            {
                l += stretchOutL[static_cast<std::size_t>(i)] * g;
                r += stretchOutR[static_cast<std::size_t>(i)] * g;
                if (useDry)
                {
                    dl += dryOutL[static_cast<std::size_t>(i)] * g;
                    dr += dryOutR[static_cast<std::size_t>(i)] * g;
                }
            }
            else if (useStems)
            {
                l += readMix(0, playhead, rate, mainFrom.data(), mainTo.data(), t, i) * g;
                r += readMix(1, playhead, rate, mainFrom.data(), mainTo.data(), t, i) * g;
                if (useDry)
                {
                    dl += readMix(0, playhead, rate, dryFrom.data(), dryTo.data(), t, i) * g;
                    dr += readMix(1, playhead, rate, dryFrom.data(), dryTo.data(), t, i) * g;
                }
            }
            else
            {
                l += readSample(0, playhead, rate) * g;
                r += readSample(1, playhead, rate) * g;
            }
            playhead += rate;
            if (mainFadeRemaining > 0)
                --mainFadeRemaining;

            if (scratch != Scratch::None) // the hand can't push past either end
                playhead = std::clamp(playhead, 0.0, len);
            else if (playhead >= len) // end of track: stop at the end (the track's own ending is the fade)
            {
                playhead = len;
                atEnd = true;
                state = Transport::Paused;
                stopKind = StopKind::None;
                mainFadeRemaining = 0;
            }
            else if (playhead < 0.0) // reverse / spin-back reached the start: stop there
            {
                playhead = 0.0;
                state = Transport::Paused;
                stopKind = StopKind::None;
                mainFadeRemaining = 0;
            }
        }

        for (auto& tail : tails)
        {
            if (!tail.active)
                continue;
            const float progress = 1.0f - static_cast<float>(tail.remaining) / static_cast<float>(fadeSamples);
            const float g = tail.startGain * std::cos(kHalfPi * progress);
            if (useStems)
            {
                l += readMix(0, tail.position, rate, mainFrom.data(), mainTo.data(), t, i) * g;
                r += readMix(1, tail.position, rate, mainFrom.data(), mainTo.data(), t, i) * g;
                if (useDry)
                {
                    dl += readMix(0, tail.position, rate, dryFrom.data(), dryTo.data(), t, i) * g;
                    dr += readMix(1, tail.position, rate, dryFrom.data(), dryTo.data(), t, i) * g;
                }
            }
            else
            {
                l += readSample(0, tail.position, rate) * g;
                r += readSample(1, tail.position, rate) * g;
            }
            tail.position += rate;
            if (--tail.remaining <= 0)
                tail.active = false;
        }

        if (dryLeft != nullptr)
        {
            dryLeft[i] = dl;
            dryRight[i] = dr;
        }
        else
        {
            l += dl;
            r += dr;
        }
        left[i] = l;
        right[i] = r;
    }

    lastRate = blockRates[static_cast<std::size_t>(numSamples - 1)];
    if (releaseFinished)
        finishScratchRelease();
    if (stopFinished && stopKind != StopKind::None) // the brake / spin-back has come to a stop
    {
        stopKind = StopKind::None;
        if (state == Transport::Playing)
        {
            state = Transport::Paused;
            stopSounding();
        }
    }
}

} // namespace devildj::engine
