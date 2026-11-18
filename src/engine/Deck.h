#pragma once

#include "dsp/Resampler.h"
#include "engine/TrackBuffer.h"

#include <array>
#include <cstdint>

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

    // Not real-time safe (builds resampler tables).
    void prepare(double sampleRate);

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

    // Tempo fader in percent; clamped to +-tempoRange().
    void setTempoPercent(double percent) noexcept;
    void nudgeTempo(double deltaPercent) noexcept;
    // 6, 10, 16 or 50 (other values snap to the nearest). The tempo is clamped to the new range.
    void setTempoRange(double rangePercent) noexcept;
    void resetTempo() noexcept { setTempoPercent(0.0); }
    // Target bend in percent (clamped to +-kMaxPitchBendPercent); the actual bend slews towards it.
    void setPitchBend(double percent) noexcept;
    void setInterpolation(dsp::Resampler::Quality quality) noexcept { resampler.setQuality(quality); }

    double tempoPercent() const noexcept { return tempo; }
    double tempoRange() const noexcept { return range; }
    double pitchBendPercent() const noexcept { return bend; }
    double rate() const noexcept { return (1.0 + tempo / 100.0) * (1.0 + bend / 100.0); }

    // Writes numSamples into left/right (overwrites).
    void render(float* left, float* right, int numSamples) noexcept;

    Transport transport() const noexcept { return state; }
    bool isPlaying() const noexcept { return state != Transport::Paused; }
    double position() const noexcept { return playhead; }
    double cuePoint() const noexcept { return cue; }
    bool isAtEnd() const noexcept { return atEnd; }
    int length() const noexcept { return buffer != nullptr ? buffer->numSamples() : 0; }

    // True when no read head is producing sound (playing heads and fading tails). A buffer may only be
    // swapped or released when the deck is silent.
    bool isSilent() const noexcept;

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

    const TrackBuffer* buffer = nullptr;
    double sampleRate = 48000.0;
    int fadeSamples = 96;

    Transport state = Transport::Paused;
    double playhead = 0.0;
    double cue = 0.0;
    bool atEnd = false;
    bool cueHeldAfterBackCue = false; // cue pressed while playing: its release must not do anything

    int mainFadeRemaining = 0; // > 0 while the main head fades in

    dsp::Resampler resampler;
    double tempo = 0.0;      // percent
    double range = 10.0;     // percent
    double bend = 0.0;       // percent, current (slewing)
    double bendTarget = 0.0; // percent

    static constexpr int kMaxTails = 4;
    std::array<Tail, kMaxTails> tails{};
};

} // namespace devildj::engine
