#pragma once

#include "dsp/fx/Effect.h"

#include <cstdint>
#include <memory>

namespace devildj::dsp::fx
{

// The beat FX set (P3-09): all our own DSP. Times are in beats of the BeatContext's tempo (1/16 ... 16 beats; a
// delay is at most 16 s). Every feedback path goes through a soft clipper, so no setting gets the output above
// +6 dBFS from a 0 dBFS input.
//
//   Echo        tempo-synced delay; feedback, tone (a low-pass in the feedback)                       tail
//   Ping Pong   the same, bouncing left / right                                                       tail
//   Reverb      8-line feedback delay network; decay, damping, size                                   tail
//   Flanger     0.5-8 ms modulated delay, one LFO cycle per beat length; depth, feedback
//   Phaser      6 all-pass stages swept per beat length; depth, feedback
//   Filter LFO  a resonant low-pass swept per beat length; depth, resonance
//   Roll        from the next beat-length boundary, that length of audio repeated
//   Trans       a beat-synced gate (on for part of each beat length); duty
//   Spiral      an echo whose time shortens repeat by repeat (its pitch rises); feedback, speed      tail
//   Bit Crush   bit depth and sample-rate reduction; bits, downsample
//   Noise       white noise through a swept band-pass, added; level, resonance
//   Pitch       pitch shift (the key-lock stretcher, transposing); semitones -12 ... +12
//   Vinyl Brake the sound slowing to a stop over the beat length (a release effect)
//   Freeze      the first beat length after switch-on (up to 4 s) held and repeated (helix)
enum class EffectType : std::uint8_t
{
    Echo,
    PingPong,
    Reverb,
    Flanger,
    Phaser,
    FilterLfo,
    Roll,
    Trans,
    Spiral,
    BitCrush,
    Noise,
    Pitch,
    VinylBrake,
    Freeze,
    Count
};
inline constexpr int kEffectTypeCount = static_cast<int>(EffectType::Count);

const char* effectTypeName(EffectType type) noexcept;
// Not real-time safe (allocates); prepare() it before handing it to the audio thread.
std::unique_ptr<Effect> makeEffect(EffectType type);

} // namespace devildj::dsp::fx
