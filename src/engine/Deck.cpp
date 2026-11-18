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
    const auto seekLength = static_cast<std::size_t>(stretcher->seekLength());
    for (auto* v : {&stretchInL, &stretchInR})
        v->assign(static_cast<std::size_t>(maxInput), 0.0f);
    for (auto* v : {&stretchOutL, &stretchOutR})
        v->assign(static_cast<std::size_t>(maxBlock), 0.0f);
    for (auto* v : {&seekL, &seekR})
        v->assign(seekLength, 0.0f);
    blockRates.assign(static_cast<std::size_t>(maxBlock), 1.0);
    stretcherPrimed = false;
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
    state = Transport::Paused;
    playhead = 0.0;
    cue = 0.0;
    atEnd = false;
    cueHeldAfterBackCue = false;
    mainFadeRemaining = 0;
    tails = {};
    stretcherPrimed = false;
}

bool Deck::isSilent() const noexcept
{
    if (state != Transport::Paused)
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
        return;
    if (atEnd)
        return; // nothing left to play; seek or cue first
    state = Transport::Playing;
    startSounding();
}

void Deck::pause() noexcept
{
    if (state == Transport::Paused)
        return;
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

void Deck::seek(double samplePosition) noexcept
{
    if (buffer == nullptr)
        return;
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

// Heard position = input fed - inputLatency - outputLatency x rate. Prime so the next output is the playhead.
void Deck::primeStretcher(double rate) noexcept
{
    const auto lead = static_cast<double>(stretcher->inputLatency()) + stretcher->outputLatency() * rate;
    const auto end = static_cast<std::int64_t>(std::llround(playhead + lead));
    const int n = stretcher->seekLength();
    readFrames(end - n, n, seekL.data(), seekR.data());
    const float* in[] = {seekL.data(), seekR.data()};
    stretcher->reset();
    stretcher->seek(in, n, rate);
    feedPosition = end;
    stretcherPrimed = true;
}

void Deck::renderStretched(int numSamples, double playheadEnd, double rateEnd) noexcept
{
    const auto lead = static_cast<double>(stretcher->inputLatency()) + stretcher->outputLatency() * rateEnd;
    const auto target = static_cast<std::int64_t>(std::llround(playheadEnd + lead));
    const int count = static_cast<int>(std::clamp<std::int64_t>(target - feedPosition, 0, maxInput));
    readFrames(feedPosition, count, stretchInL.data(), stretchInR.data());
    feedPosition += count;

    const float* in[] = {stretchInL.data(), stretchInR.data()};
    float* out[] = {stretchOutL.data(), stretchOutR.data()};
    stretcher->process(in, count, out, numSamples);
}

void Deck::render(float* left, float* right, int numSamples) noexcept
{
    std::fill(left, left + numSamples, 0.0f);
    std::fill(right, right + numSamples, 0.0f);
    if (buffer == nullptr)
        return;

    if (numSamples > maxBlock) // larger than prepared: split (keeps the scratch buffers big enough)
    {
        for (int done = 0; done < numSamples; done += maxBlock)
            render(left + done, right + done, std::min(maxBlock, numSamples - done));
        return;
    }

    const double len = length();
    const double bendStep = kPitchBendSlewPercentPerSecond / sampleRate;

    // Rates for this block (the bend slews per sample), so key lock knows how far the playhead will move.
    double playheadEnd = playhead;
    for (int i = 0; i < numSamples; ++i)
    {
        if (bend != bendTarget)
            bend = bend < bendTarget ? std::min(bendTarget, bend + bendStep) : std::max(bendTarget, bend - bendStep);
        blockRates[static_cast<std::size_t>(i)] = this->rate();
        playheadEnd += blockRates[static_cast<std::size_t>(i)];
    }

    const bool stretched = keyLockOn && state != Transport::Paused && stretcher != nullptr;
    if (stretched)
    {
        if (!stretcherPrimed)
            primeStretcher(blockRates[0]);
        renderStretched(numSamples, playheadEnd, blockRates[static_cast<std::size_t>(numSamples - 1)]);
    }

    for (int i = 0; i < numSamples; ++i)
    {
        float l = 0.0f, r = 0.0f;
        const double rate = blockRates[static_cast<std::size_t>(i)];

        if (state != Transport::Paused)
        {
            const float g = currentMainGain();
            if (stretched)
            {
                l += stretchOutL[static_cast<std::size_t>(i)] * g;
                r += stretchOutR[static_cast<std::size_t>(i)] * g;
            }
            else
            {
                l += readSample(0, playhead, rate) * g;
                r += readSample(1, playhead, rate) * g;
            }
            playhead += rate;
            if (mainFadeRemaining > 0)
                --mainFadeRemaining;

            if (playhead >= len) // end of track: stop at the end (the track's own ending is the fade)
            {
                playhead = len;
                atEnd = true;
                state = Transport::Paused;
                mainFadeRemaining = 0;
            }
        }

        for (auto& t : tails)
        {
            if (!t.active)
                continue;
            const float progress = 1.0f - static_cast<float>(t.remaining) / static_cast<float>(fadeSamples);
            const float g = t.startGain * std::cos(kHalfPi * progress);
            l += readSample(0, t.position, rate) * g;
            r += readSample(1, t.position, rate) * g;
            t.position += rate;
            if (--t.remaining <= 0)
                t.active = false;
        }

        left[i] = l;
        right[i] = r;
    }
}

} // namespace devildj::engine
