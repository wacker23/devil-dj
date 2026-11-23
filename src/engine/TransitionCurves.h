#pragma once

// Transition curves (P4-14): what each deck does during a transition, as automation lanes of breakpoints.
//
// Beats are counted from the transition's start (0 .. beats). Each lane holds points sorted by beat; a point's shape
// is how the value goes from it to the next point. Before the first point a lane holds the first value, after the
// last one the last value. An empty lane leaves its parameter alone.
//
// Lanes and their values:
//   Volume  0..1     a gain after the channel fader (1 = as the fader is)
//   EqLow / EqMid / EqHigh   dB, -26..+6 (<= -100 = kill)
//   Filter  -1..1    -1 low-pass fully closed, 0 off, +1 high-pass fully closed (the channel's filter knob)
//   FxSend  0..1     the transition's effect (P4-18)
//
// Built on the message thread, immutable afterwards, read by the audio thread through a pointer (no allocation
// there). The engine keeps the last few alive (AudioEngine::startTransition).

#include <array>
#include <cstdint>
#include <vector>

namespace devildj::engine
{

enum class Lane : std::uint8_t
{
    Volume,
    EqLow,
    EqMid,
    EqHigh,
    Filter,
    FxSend,
};
constexpr int kLaneCount = 6;

enum class Shape : std::uint8_t
{
    Linear,
    EaseIn,  // slow, then fast (t^2)
    EaseOut, // fast, then slow
    SCurve,  // smoothstep
    Hold,    // stays until the next point, then jumps
};

struct CurvePoint
{
    float beat = 0.0f;
    float value = 0.0f;
    Shape shape = Shape::Linear;
};

struct LaneCurve
{
    std::vector<CurvePoint> points; // sorted by beat; empty = the lane isn't used
    bool used() const noexcept { return !points.empty(); }
};

struct TransitionCurves
{
    float beats = 16.0f; // its length
    float loopBeats = 0.0f; // P4-18: the outgoing deck loops its last this-many beats through it (0: no loop)
    std::array<LaneCurve, kLaneCount> from, to; // the outgoing deck's lanes, the incoming deck's

    LaneCurve& lane(bool incoming, Lane l) { return (incoming ? to : from)[static_cast<std::size_t>(l)]; }
    const LaneCurve& lane(bool incoming, Lane l) const { return (incoming ? to : from)[static_cast<std::size_t>(l)]; }
};

// A lane's value at `beat` (no allocation; safe on the audio thread).
float evaluate(const LaneCurve& lane, double beat) noexcept;
// The value a lane means "untouched": volume 1, EQ 0 dB, filter 0, FX 0.
float neutralValue(Lane lane) noexcept;
// Points sorted, values clamped to their lane's range, beats clamped to 0 .. beats.
void normalize(TransitionCurves& curves);

} // namespace devildj::engine
