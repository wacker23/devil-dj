#pragma once

#include "dsp/LinearRamp.h"
#include "dsp/fx/Effect.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace devildj::dsp::fx
{

// Release FX (P3-11): hold to apply, let go to cut the dry signal - the sound ends on the effect over the beat
// length. One per channel (after the color FX). Audio thread; prepare allocates.
//
//   Echo Out      held: an echo of the beat length on the music; let go: the music is cut (10 ms), the echo
//                 rings out (feedback 0.65) until it has died away (or 8 beat lengths)
//   Brake         held: nothing yet; let go: the sound slows to a stop over the beat length
//   Spin Back     held: nothing yet; let go: the sound spins backwards (3x) and slows to a stop
//   Filter Sweep  held: a high-pass rising over the beat length (20 Hz -> 1.5 kHz); let go: it rises on to
//                 15 kHz and fades out over the beat length
//
// finished() goes true once the release is over: the channel is then silent until reset() (the engine pauses the
// deck and resets it). Idle = the signal untouched, bit-exact.
class ReleaseFx
{
public:
    enum class Kind : std::uint8_t
    {
        EchoOut,
        Brake,
        SpinBack,
        FilterSweep,
        Count
    };
    static constexpr int kKindCount = static_cast<int>(Kind::Count);
    static const char* kindName(Kind kind) noexcept;
    enum class Stage : std::uint8_t
    {
        Idle,
        Held,
        Released,
        Finished,
    };

    ReleaseFx();
    ~ReleaseFx();
    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept; // Idle

    void press(Kind kind, double beats) noexcept; // ignored unless Idle
    void release() noexcept;                      // Held -> Released
    Stage stage() const noexcept { return current; }
    Kind kind() const noexcept { return currentKind; }
    bool finished() const noexcept { return current == Stage::Finished; }

    void process(float* left, float* right, int numSamples, const BeatContext& beat) noexcept;

private:
    double lengthSamples(const BeatContext& beat) const noexcept;
    float hp(int channel, float x, double hz) noexcept;

    double sampleRate = 48000.0;
    Stage current = Stage::Idle;
    Kind currentKind = Kind::EchoOut;
    double beats = 1.0;
    std::unique_ptr<Effect> echo;
    std::vector<float> wetL, wetR;
    // The last 16 s of what went through (also while idle), for brake / spin-back to play from.
    std::vector<float> histL, histR;
    std::size_t write = 0, filled = 0;
    double readBack = 0.0;  // samples behind the newest, while releasing
    double elapsed = 0.0;   // samples since the press / the release
    LinearRamp dry;         // 1 -> 0 on release
    int silentSamples = 0;
    double hpIc1[2]{}, hpIc2[2]{};
};

} // namespace devildj::dsp::fx
