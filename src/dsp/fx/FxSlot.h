#pragma once

#include "dsp/LinearRamp.h"
#include "dsp/fx/Effect.h"

#include <vector>

namespace devildj::dsp::fx
{

// One effect in an FX unit (P3-08): dry / wet, on / off, the tail. Audio thread (prepare: not real-time safe).
//
//   out = (1 - wet x m) x in + wet x g x effect(m x in)
//
// m is the send, ramped 0 <-> 1 over 20 ms when the slot is switched on / off; g = 1 for an effect with a tail
// (it starts from silence and keeps sounding after the send is closed) and g = m for one without (it fades in and
// out with the send).
// Stages:
//   Off       - nothing is touched: the signal passes bit-exact (also with no effect in the slot)
//   On        - sending (m ramps up to 1 when switched on)
//   FadingOut - switched off, m ramping down to 0
//   Tail      - (effects with a tail) the send is closed, the effect runs on silence and its output is still
//               mixed in, until it has been below -100 dBFS for 50 ms (or 30 s at most); then reset and Off.
// Switching on again during FadingOut or Tail ramps the send back up without resetting the effect.
class FxSlot
{
public:
    enum class Stage
    {
        Off,
        On,
        FadingOut,
        Tail,
    };

    static constexpr double kFadeSeconds = 0.02;
    static constexpr float kSilence = 1.0e-5f; // -100 dBFS
    static constexpr double kSilentSeconds = 0.05;
    static constexpr double kMaxTailSeconds = 30.0;

    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept; // Off, the effect cleared

    // The effect (prepared by the caller; nullptr = an empty slot). The slot goes Off at once.
    void setEffect(Effect* effect) noexcept;
    Effect* effect() const noexcept { return fx; }

    void setOn(bool on) noexcept;
    bool isOn() const noexcept { return requestedOn; }
    void setWet(float wet) noexcept; // 0..1, smoothed
    float wet() const noexcept { return wetTarget; }

    Stage stage() const noexcept { return current; }
    bool isActive() const noexcept { return current != Stage::Off; }

    void process(float* left, float* right, int numSamples, const BeatContext& beat) noexcept;

private:
    Effect* fx = nullptr;
    bool requestedOn = false;
    Stage current = Stage::Off;
    float wetTarget = 1.0f;
    LinearRamp send, wetRamp;
    int silentSamples = 0, tailSamples = 0;
    int silentLimit = 2400, tailLimit = 1440000;
    std::vector<float> wetL, wetR;
};

} // namespace devildj::dsp::fx
