#pragma once

#include "dsp/LinearRamp.h"
#include "dsp/Stretcher.h"
#include "dsp/fx/Effect.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace devildj::dsp::fx
{

// Color FX (P3-10): one knob per channel, its character chosen once for all channels. Centre (+-2 %) = off; the
// two sides are two characters of the same effect; the further from the centre, the more of it.
//
//   Filter    the channel strip's own filter (engine::ChannelStrip; this class leaves the signal alone)
//   Space     reverb send: left dark (low-passed), right airy (high-passed); it rings out at the centre
//   Dub Echo  a 3/4-beat echo with long feedback: left dark, right bright; it rings out at the centre
//   Noise     noise rising with the knob; left closes a low-pass on the signal, right opens a high-pass
//   Crush     left fewer bits, right a lower sample rate
//   Pitch     left down, right up, up to an octave (continuous)
//
// The amount follows the knob over 20 ms; changing the character fades the old one out (20 ms) before the new one
// comes in. Off (centre, nothing ringing) = the signal untouched, bit-exact. Audio thread; prepare allocates.
class ColorFx
{
public:
    enum class Type : std::uint8_t
    {
        Filter,
        Space,
        DubEcho,
        Noise,
        Crush,
        Pitch,
        Count
    };
    static constexpr int kTypeCount = static_cast<int>(Type::Count);
    static const char* typeName(Type type) noexcept;
    static constexpr double kDeadZone = 0.02;
    static constexpr double kSmoothingSeconds = 0.02;

    ColorFx();
    ~ColorFx();
    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;

    void setType(Type type) noexcept;
    Type type() const noexcept { return wanted; }
    void setKnob(double knob) noexcept; // 0..1, 0.5 = off
    double knob() const noexcept { return knobValue; }
    // -1 (full left) .. +1 (full right), 0 inside the dead zone.
    static double signedAmount(double knob) noexcept;

    bool isActive() const noexcept { return active; }
    void process(float* left, float* right, int numSamples, const BeatContext& beat) noexcept;

    // The smoothed amount now (for tests).
    double currentAmount() const noexcept { return amount.value(); }

private:
    struct Filter2 // a 12 dB TPT state-variable filter, low- or high-pass
    {
        double ic1 = 0.0, ic2 = 0.0;
        void reset() noexcept { ic1 = ic2 = 0.0; }
        float process(float x, double g, bool highPass) noexcept;
    };
    double gFor(double hz) const noexcept;
    void startType() noexcept;   // the wanted type becomes the current one
    void clearEffects() noexcept; // their state (delay lines, the pitch shifter) back to silence
    bool ringing(const float* l, const float* r, int n) noexcept;

    double sampleRate = 48000.0;
    Type current = Type::Filter, wanted = Type::Filter;
    double knobValue = 0.5;
    LinearRamp amount;   // signed, -1..1
    LinearRamp outGain;  // 1, or down to 0 while changing type
    bool active = false;
    int silentSamples = 0;
    std::unique_ptr<Effect> reverb, echo, noise, crush;
    std::unique_ptr<Stretcher> pitch;
    double pitchShown = 1000.0;
    std::vector<float> pitchPrime;
    std::vector<float> wetL, wetR, outL, outR;
    Filter2 fl, fr;
};

} // namespace devildj::dsp::fx
