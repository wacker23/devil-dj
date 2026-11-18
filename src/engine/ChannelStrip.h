#pragma once

#include "dsp/Biquad.h"
#include "dsp/LinearRamp.h"
#include "dsp/StateVariableFilter.h"

#include <array>
#include <cstdint>
#include <limits>

namespace devildj::engine
{

// One mixer channel (ARCHITECTURE §3.8): trim -> EQ / isolator -> filter -> fader. Stereo, in place.
//
// EQ mode: RBJ low shelf 70 Hz, peak 1 kHz, high shelf 13 kHz (Q 0.7), -26...+6 dB per band.
// Isolator mode: Linkwitz-Riley 8th-order crossovers (48 dB/oct) at 250 Hz and 2.5 kHz, low band through the
//   matching all-pass at 2.5 kHz, so the three bands add back up flat; per band -inf (kill)...+6 dB.
//   (LR8 rather than LR4 so a kill takes more than 60 dB off at the band centre; LR4 manages ~40 dB.)
// Filter: one knob, 0..1. Centre (+-2 %) = off; left = resonant low-pass 20 kHz -> 60 Hz, right = high-pass
//   20 Hz -> 10 kHz (both exponential), TPT state-variable filters; low-pass and high-pass have their own
//   filter so crossing the centre crossfades instead of sweeping through the whole range.
// Fader: 0..1 with a selectable curve.
// Every parameter moves over 20 ms; switching EQ mode crossfades the two paths over 20 ms.
class ChannelStrip
{
public:
    enum class EqMode : std::uint8_t
    {
        Eq,
        Isolator,
    };
    enum class FaderCurve : std::uint8_t
    {
        Smooth, // x^2: -12 dB at half way (default)
        Linear, // x: -6 dB at half way
        Sharp,  // 1 - (1 - x)^3: nearly full level from a third of the way
    };
    enum Band
    {
        Low,
        Mid,
        High,
    };
    static constexpr int kBands = 3;

    static constexpr double kTrimMaxDb = 12.0;
    static constexpr double kEqMinDb = -26.0;
    static constexpr double kBandMaxDb = 6.0;
    static constexpr double kKillDb = -std::numeric_limits<double>::infinity();
    static constexpr double kSmoothingSeconds = 0.02;
    static constexpr double kEqLowHz = 70.0, kEqMidHz = 1000.0, kEqHighHz = 13000.0, kEqQ = 0.7;
    static constexpr double kCrossoverLowHz = 250.0, kCrossoverHighHz = 2500.0;
    static constexpr double kFilterDeadZone = 0.02;
    static constexpr double kFilterDefaultResonance = 0.3;

    void prepare(double sampleRate, int maxBlockSize) noexcept;
    void reset() noexcept;
    void process(float* left, float* right, int numSamples) noexcept; // = processPreFader + applyFader
    // Split for taps between the two (channel meters, and the cue bus in P1-12).
    void processPreFader(float* left, float* right, int numSamples) noexcept;
    void applyFader(float* left, float* right, int numSamples) noexcept;

    void setTrimDb(double db) noexcept;
    void setEqMode(EqMode mode) noexcept;
    void setBandDb(int band, double db) noexcept;    // kKillDb (or anything below -100) = kill
    void setFilter(double knob) noexcept;            // 0..1, 0.5 = off
    void setFilterResonance(double amount) noexcept; // 0..1
    void setFader(double position) noexcept;         // 0..1
    void setFaderCurve(FaderCurve curve) noexcept;

    double trimDb() const noexcept { return trim; }
    EqMode eqMode() const noexcept { return mode; }
    double bandDb(int band) const noexcept { return bands[static_cast<std::size_t>(band)]; }
    double filter() const noexcept { return filterKnob; }
    double filterResonance() const noexcept { return resonance; }
    double fader() const noexcept { return faderPosition; }
    FaderCurve faderCurve() const noexcept { return curve; }

    static double faderGain(double position, FaderCurve curve) noexcept;

    // Where the filter knob puts the two filters, and how much of each is heard.
    struct FilterTarget
    {
        double lowPassHz, highPassHz, dry, lowPass, highPass;
    };
    static FilterTarget filterTarget(double knob) noexcept;
    static double filterQ(double resonance) noexcept;

private:
    using Stereo = std::array<dsp::BiquadState, 2>;

    void updateEqCoeffs() noexcept;
    void updateFilterTargets() noexcept;
    void updateFaderTarget() noexcept;
    double processEq(int ch, double x) noexcept;
    double processIsolator(int ch, double x, double gLow, double gMid, double gHigh) noexcept;

    double sampleRate = 48000.0;

    // User values (what the knobs say).
    double trim = 0.0;
    EqMode mode = EqMode::Eq;
    std::array<double, kBands> bands{};
    double filterKnob = 0.5;
    double resonance = kFilterDefaultResonance;
    double faderPosition = 1.0;
    FaderCurve curve = FaderCurve::Smooth;

    // Smoothed values.
    dsp::LinearRamp trimGain, faderGainRamp, modeMix; // modeMix: 0 = EQ, 1 = isolator
    std::array<dsp::LinearRamp, kBands> eqDb, isoGain;
    dsp::LinearRamp lowPassLog2Hz, highPassLog2Hz, dryWeight, lowPassWeight, highPassWeight, filterQRamp;

    // EQ mode filters.
    std::array<dsp::BiquadCoeffs, kBands> eqCoeffs;
    std::array<Stereo, kBands> eqState;

    // Isolator: LR8 = Butterworth 4th order (2 sections) twice.
    std::array<dsp::BiquadCoeffs, 2> lp250, hp250, lp2500, hp2500, ap2500;
    std::array<Stereo, 4> lowSections, restSections, midSections, highSections;
    std::array<Stereo, 2> lowAllPass;

    // Channel filter.
    std::array<dsp::SvfState, 2> lowPassState, highPassState;
    dsp::SvfCoeffs lowPassCoeffs, highPassCoeffs;
    bool filterRunning = false;
    bool eqRunning = true, isolatorRunning = false;
};

} // namespace devildj::engine
