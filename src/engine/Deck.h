#pragma once

#include "dsp/Resampler.h"
#include "dsp/Stretcher.h"
#include "engine/TrackBuffer.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace devildj::engine
{

// One deck's transport (P1-06; ARCHITECTURE §3.4). Audio thread only: no allocation, no locks.
//
// Transport: play, pause, seek, CDJ-style cue, end of track. Position is a double in samples of the loaded
// TrackBuffer (already at the device rate).
//
// Tempo (P1-07): rate = (1 + tempo %) x (1 + pitch bend %). The tempo fader has ranges of +-6/10/16/50 %; fine
// steps nudge it; pitch bend is a temporary +-4 % that slews (40 %/s) so it never jumps. Without key lock the
// track is read at that rate through dsp::Resampler (cubic Hermite, or 16-tap sinc in high-quality mode), so
// pitch follows tempo like a turntable. Tempo survives loading a new track, like a CDJ's tempo fader.
//
// Key lock (P1-08): the main read head goes through dsp::Stretcher instead, so tempo changes keep the pitch;
// Key Shift transposes in semitones on top. The stretcher delays audio; the deck feeds it ahead of the
// playhead and `position()` stays what the listener hears. Jump fades still work: the old position fades out
// through the resampler while the stretched audio fades in.
//
// Jog (P1-09): scratchBegin/Move/End follow the hand like vinyl. Each move says how far the hand went since
// the previous one and when (UI clock). The deck plays at the hand's speed over the last 4 moves (timed on the
// UI clock, so it doesn't matter which audio block picks a move up), aiming one move behind the hand and
// correcting position drift over 100 ms; a steady hand at 60 events/s gives a constant rate instead of 60 Hz
// steps ("zipper"). When moves stop, the deck settles onto the hand within ~20 ms; a 5 ms smoother removes
// what's left. Key lock is bypassed while
// scratching. Playing: release eases back to the normal rate over 50 ms. Paused: dragging scrubs (audible) and
// the deck stays paused on release. nudge() is the scroll-wheel fine nudge: a small position shift played
// out as a <= 5 % rate offset while playing, or moved silently while paused.
//
// CDJ cue button:
//   paused, cue pressed   -> the cue point moves to the playhead, and playback previews from it while held
//   preview, cue released -> back to the cue point, paused
//   preview, play pressed -> keeps playing (releasing cue no longer returns)
//   playing, cue pressed  -> back to the cue point, paused ("back cue"); releasing does nothing
//
// Declicking: every discontinuity (seek or cue jump while sounding, play, pause, end of track) uses a 2 ms
// equal-power fade. A jump keeps the old read head sounding as a fading "tail" while the new one fades in.
class Deck
{
public:
    enum class Transport : std::uint8_t
    {
        Paused,
        Playing,
        CuePreview, // cue button held from pause: plays from the cue point until released
    };

    static constexpr double kFadeSeconds = 0.002;
    static constexpr double kMaxPitchBendPercent = 4.0;
    static constexpr double kPitchBendSlewPercentPerSecond = 40.0;

    // Not real-time safe (builds resampler tables, the stretcher and its buffers).
    void prepare(double sampleRate, int maxBlockSize = 4096);

    // Puts a new track on the deck (or nullptr to empty it): paused at 0, cue at 0. The caller must make sure
    // nothing still reads the old buffer (see isSilent()).
    void setTrack(const TrackBuffer* track) noexcept;
    const TrackBuffer* track() const noexcept { return buffer; }

    void play() noexcept;
    void pause() noexcept;
    void togglePlay() noexcept;
    void cuePress() noexcept;
    void cueRelease() noexcept;
    void seek(double samplePosition) noexcept; // clamped to [0, length]
    void setCuePoint(double samplePosition) noexcept; // P3-02: a memory cue called; clamped to [0, length]

    // Loop (P3-03): while active and playing forward, the playhead wraps from `end` back to `start` at the exact
    // sample it reaches the end (the overshoot carried, so the timing stays sample-accurate), through the usual
    // 2 ms crossfade. A playhead before the start plays into the loop; one past the end wraps at once (by the
    // overshoot modulo the loop length). Works with key lock too (the block is split at the wrap).
    void setLoop(double start, double end) noexcept; // activates it (end > start, within the track)
    void exitLoop() noexcept;                         // the loop stays known (for reloop), playback goes on
    bool loopActive() const noexcept { return loopOn; }
    double loopStart() const noexcept { return loopIn; }
    double loopEnd() const noexcept { return loopOut; }

    // Tempo fader in percent; clamped to +-tempoRange().
    void setTempoPercent(double percent) noexcept;
    void nudgeTempo(double deltaPercent) noexcept;
    // 6, 10, 16 or 50 (other values snap to the nearest). The tempo is clamped to the new range.
    void setTempoRange(double rangePercent) noexcept;
    void resetTempo() noexcept { setTempoPercent(0.0); }
    // Sync (P2-19): the tempo a sync needs; the fader range widens (6 -> 10 -> 16 -> 50 %) if it doesn't fit.
    void setSyncTempo(double percent) noexcept;
    // Sync's phase correction: a factor on the rate, within 1 +- kMaxRateCorrection.
    void setRateCorrection(double factor) noexcept;
    double rateCorrection() const noexcept { return correction; }
    static constexpr double kMaxRateCorrection = 0.005;
    // Target bend in percent (clamped to +-kMaxPitchBendPercent); the actual bend slews towards it.
    void setPitchBend(double percent) noexcept;
    void setInterpolation(dsp::Resampler::Quality quality) noexcept { resampler.setQuality(quality); }

    // Jog / scratch (see top). Deltas are in samples of the track (signed).
    // timeSeconds: UI event time on any steady clock (negative = unknown, use the audio clock).
    void scratchBegin(double timeSeconds = -1.0) noexcept;
    void scratchMove(double deltaSamples, double timeSeconds = -1.0) noexcept;
    void scratchEnd() noexcept;
    void nudge(double deltaSamples) noexcept;
    bool isScratching() const noexcept { return scratch != Scratch::None; }

    static constexpr double kMaxScratchRate = 8.0;
    static constexpr double kScratchReleaseSeconds = 0.05;
    static constexpr double kMaxNudgeRateOffset = 0.05;

    // Key lock on/off (crossfades if playing) and Key Shift in semitones (only heard with key lock on).
    void setKeyLock(bool on) noexcept;
    void setKeyShift(int semitones) noexcept;
    bool keyLock() const noexcept { return keyLockOn; }
    int keyShift() const noexcept { return keyShiftSemitones; }

    double tempoPercent() const noexcept { return tempo; }
    double tempoRange() const noexcept { return range; }
    double pitchBendPercent() const noexcept { return bend; }
    double rate() const noexcept { return (1.0 + tempo / 100.0) * (1.0 + bend / 100.0) * correction; }

    // Writes numSamples into left/right (overwrites).
    void render(float* left, float* right, int numSamples) noexcept;

    Transport transport() const noexcept { return state; }
    bool isPlaying() const noexcept { return state != Transport::Paused; }
    // Rate of the last rendered sample (includes scratching and nudges; rate() is the tempo setting).
    double currentRate() const noexcept { return lastRate; }
    double position() const noexcept { return playhead; }
    double cuePoint() const noexcept { return cue; }
    bool isAtEnd() const noexcept { return atEnd; }
    int length() const noexcept { return buffer != nullptr ? buffer->numSamples() : 0; }

    // True when no read head is producing sound (playing heads and fading tails). A buffer may only be
    // swapped or released when the deck is silent.
    bool isSilent() const noexcept;

private:
    enum class Scratch : std::uint8_t
    {
        None,
        Holding,   // hand on the "record"
        Releasing, // let go while playing: easing back to the normal rate
    };
    double nextScratchRate(double playheadNow, double normalRate) noexcept;
    void finishScratchRelease() noexcept;

    Scratch scratch = Scratch::None;
    double scratchTarget = 0.0;  // hand position (samples)
    double scratchRate = 0.0;    // current, smoothed
    double scratchDesired = 0.0; // rate implied by the latest move
    double moveInterval = 800.0; // smoothed samples between moves
    double samplesSinceMove = 0.0;
    // Hand speed = distance over the last kMoveWindow moves / their total time: event-timing jitter (UI thread,
    // block boundaries) mostly cancels out, unlike a per-move estimate.
    static constexpr int kMoveWindow = 4;
    std::array<double, kMoveWindow> moveDeltas{};
    std::array<double, kMoveWindow> moveIntervals{};
    int moveCount = 0;
    double lastMoveTime = -1.0; // UI clock, seconds
    int moveIndex = 0;
    int releaseRemaining = 0;
    double releaseFromRate = 0.0;
    double nudgeRemaining = 0.0; // samples of position still to add
    double lastRate = 1.0;

public:
private:
    struct Tail // a read head that fades out after a jump or stop
    {
        bool active = false;
        double position = 0.0;
        int remaining = 0;
        float startGain = 0.0f;
    };

    void startSounding() noexcept;               // fade the main head in from silence
    void stopSounding() noexcept;                // turn the main head into a fading tail
    void jumpTo(double samplePosition) noexcept; // move the playhead; crossfade if sounding
    void spawnTail(double position, float gain) noexcept;
    float currentMainGain() const noexcept;
    float readSample(int channel, double position, double rate) const noexcept;
    void primeStretcher(double rate) noexcept;
    void renderStretched(int numSamples, double playheadEnd, double rateEnd) noexcept;
    void readFrames(std::int64_t from, int count, float* left, float* right) const noexcept;

    const TrackBuffer* buffer = nullptr;
    double sampleRate = 48000.0;
    int fadeSamples = 96;

    Transport state = Transport::Paused;
    double playhead = 0.0;
    double cue = 0.0;
    bool atEnd = false;
    bool cueHeldAfterBackCue = false; // cue pressed while playing: its release must not do anything
    bool loopOn = false;
    double loopIn = 0.0, loopOut = 0.0;
    void wrapLoop() noexcept;
    void renderPart(float* left, float* right, int numSamples) noexcept;

    int mainFadeRemaining = 0; // > 0 while the main head fades in

    dsp::Resampler resampler;
    double tempo = 0.0;      // percent
    double correction = 1.0; // sync's phase nudge (P2-19)
    double range = 10.0;     // percent
    double bend = 0.0;       // percent, current (slewing)
    double bendTarget = 0.0; // percent

    // Key lock.
    std::unique_ptr<dsp::Stretcher> stretcher;
    bool keyLockOn = false;
    int keyShiftSemitones = 0;
    bool stretcherPrimed = false;
    std::int64_t feedPosition = 0; // next input sample to hand to the stretcher
    int maxBlock = 4096;
    int maxInput = 8192;
    std::vector<float> stretchInL, stretchInR, stretchOutL, stretchOutR, seekL, seekR;
    std::vector<double> blockRates; // per-sample rate of the current block

    static constexpr int kMaxTails = 4;
    std::array<Tail, kMaxTails> tails{};
};

} // namespace devildj::engine
