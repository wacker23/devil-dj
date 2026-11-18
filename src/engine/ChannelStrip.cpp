#include "ChannelStrip.h"

#include <algorithm>
#include <cmath>

namespace devildj::engine
{

namespace
{
constexpr int kCoeffInterval = 16; // EQ coefficients are recomputed this often while a band is moving
constexpr double kLowPassMaxHz = 20000.0, kLowPassMinHz = 60.0;
constexpr double kHighPassMinHz = 20.0, kHighPassMaxHz = 10000.0;

double dbToGain(double db) noexcept
{
    return db <= -100.0 ? 0.0 : std::pow(10.0, db / 20.0);
}
} // namespace

double ChannelStrip::faderGain(double position, FaderCurve fc) noexcept
{
    const double x = std::clamp(position, 0.0, 1.0);
    switch (fc)
    {
        case FaderCurve::Linear:
            return x;
        case FaderCurve::Sharp:
            return 1.0 - (1.0 - x) * (1.0 - x) * (1.0 - x);
        case FaderCurve::Smooth:
        default:
            return x * x;
    }
}

ChannelStrip::FilterTarget ChannelStrip::filterTarget(double knob) noexcept
{
    const double x = std::clamp(knob, 0.0, 1.0);
    constexpr double edge = 0.5 - kFilterDeadZone; // 0.48: the sweep runs over the remaining 0.48 of each side
    FilterTarget t{kLowPassMaxHz, kHighPassMinHz, 1.0, 0.0, 0.0};
    if (x < 0.5 - kFilterDeadZone)
    {
        const double amount = (edge - x) / edge;
        t.lowPassHz = kLowPassMaxHz * std::pow(kLowPassMinHz / kLowPassMaxHz, amount);
        t.dry = 0.0;
        t.lowPass = 1.0;
    }
    else if (x > 0.5 + kFilterDeadZone)
    {
        const double amount = (x - (0.5 + kFilterDeadZone)) / edge;
        t.highPassHz = kHighPassMinHz * std::pow(kHighPassMaxHz / kHighPassMinHz, amount);
        t.dry = 0.0;
        t.highPass = 1.0;
    }
    return t;
}

double ChannelStrip::filterQ(double amount) noexcept
{
    return 0.70710678118654752 * std::exp2(4.0 * std::clamp(amount, 0.0, 1.0)); // 0.71 ... 11.3
}

void ChannelStrip::prepare(double newSampleRate, int /*maxBlockSize*/) noexcept
{
    sampleRate = newSampleRate;
    for (auto* r : {&trimGain, &faderGainRamp, &modeMix, &lowPassLog2Hz, &highPassLog2Hz, &dryWeight, &lowPassWeight,
                    &highPassWeight, &filterQRamp})
        r->prepare(sampleRate, kSmoothingSeconds);
    for (auto& r : eqDb)
        r.prepare(sampleRate, kSmoothingSeconds);
    for (auto& r : isoGain)
        r.prepare(sampleRate, kSmoothingSeconds);

    for (int s = 0; s < 2; ++s)
    {
        const double q = dsp::kButterworth4Q[s];
        const auto i = static_cast<std::size_t>(s);
        lp250[i] = dsp::BiquadCoeffs::lowPass(kCrossoverLowHz, sampleRate, q);
        hp250[i] = dsp::BiquadCoeffs::highPass(kCrossoverLowHz, sampleRate, q);
        lp2500[i] = dsp::BiquadCoeffs::lowPass(kCrossoverHighHz, sampleRate, q);
        hp2500[i] = dsp::BiquadCoeffs::highPass(kCrossoverHighHz, sampleRate, q);
        ap2500[i] = dsp::BiquadCoeffs::allPass(kCrossoverHighHz, sampleRate, q);
    }
    reset();
}

void ChannelStrip::reset() noexcept
{
    trimGain.snap(dbToGain(trim));
    faderGainRamp.snap(faderGain(faderPosition, curve));
    modeMix.snap(mode == EqMode::Isolator ? 1.0 : 0.0);
    eqRunning = mode == EqMode::Eq;
    isolatorRunning = mode == EqMode::Isolator;
    for (std::size_t b = 0; b < kBands; ++b)
    {
        eqDb[b].snap(std::clamp(bands[b], kEqMinDb, kBandMaxDb));
        isoGain[b].snap(dbToGain(std::min(bands[b], kBandMaxDb)));
    }
    const auto t = filterTarget(filterKnob);
    lowPassLog2Hz.snap(std::log2(t.lowPassHz));
    highPassLog2Hz.snap(std::log2(t.highPassHz));
    dryWeight.snap(t.dry);
    lowPassWeight.snap(t.lowPass);
    highPassWeight.snap(t.highPass);
    filterQRamp.snap(filterQ(resonance));
    lowPassCoeffs = dsp::SvfCoeffs::make(t.lowPassHz, sampleRate, filterQRamp.value());
    highPassCoeffs = dsp::SvfCoeffs::make(t.highPassHz, sampleRate, filterQRamp.value());
    filterRunning = t.dry < 1.0;
    updateEqCoeffs();

    for (auto* group : {&eqState})
        for (auto& st : *group)
            for (auto& s : st)
                s.reset();
    for (auto* group : {&lowSections, &restSections, &midSections, &highSections})
        for (auto& st : *group)
            for (auto& s : st)
                s.reset();
    for (auto& st : lowAllPass)
        for (auto& s : st)
            s.reset();
    for (auto& s : lowPassState)
        s.reset();
    for (auto& s : highPassState)
        s.reset();
}

void ChannelStrip::setTrimDb(double db) noexcept
{
    trim = std::clamp(db, -kTrimMaxDb, kTrimMaxDb);
    trimGain.setTarget(dbToGain(trim));
}

void ChannelStrip::setEqMode(EqMode newMode) noexcept
{
    if (newMode == mode)
        return;
    mode = newMode;
    // Bring the other path in from silence (fresh state), crossfading over 20 ms.
    if (mode == EqMode::Isolator && !isolatorRunning)
    {
        for (auto* group : {&lowSections, &restSections, &midSections, &highSections})
            for (auto& st : *group)
                for (auto& s : st)
                    s.reset();
        for (auto& st : lowAllPass)
            for (auto& s : st)
                s.reset();
        isolatorRunning = true;
    }
    if (mode == EqMode::Eq && !eqRunning)
    {
        for (auto& st : eqState)
            for (auto& s : st)
                s.reset();
        eqRunning = true;
    }
    modeMix.setTarget(mode == EqMode::Isolator ? 1.0 : 0.0);
}

void ChannelStrip::setBandDb(int band, double db) noexcept
{
    if (band < 0 || band >= kBands)
        return;
    const auto b = static_cast<std::size_t>(band);
    bands[b] = db <= -100.0 ? kKillDb : std::min(db, kBandMaxDb);
    eqDb[b].setTarget(std::clamp(bands[b], kEqMinDb, kBandMaxDb));
    isoGain[b].setTarget(dbToGain(bands[b]));
}

void ChannelStrip::setFilter(double knob) noexcept
{
    filterKnob = std::clamp(knob, 0.0, 1.0);
    updateFilterTargets();
}

void ChannelStrip::setFilterResonance(double amount) noexcept
{
    resonance = std::clamp(amount, 0.0, 1.0);
    filterQRamp.setTarget(filterQ(resonance));
}

void ChannelStrip::setFader(double position) noexcept
{
    faderPosition = std::clamp(position, 0.0, 1.0);
    updateFaderTarget();
}

void ChannelStrip::setFaderCurve(FaderCurve newCurve) noexcept
{
    curve = newCurve;
    updateFaderTarget();
}

void ChannelStrip::updateFaderTarget() noexcept
{
    faderGainRamp.setTarget(faderGain(faderPosition, curve));
}

void ChannelStrip::updateFilterTargets() noexcept
{
    const auto t = filterTarget(filterKnob);
    if (!filterRunning && t.dry < 1.0)
    {
        for (auto& s : lowPassState)
            s.reset();
        for (auto& s : highPassState)
            s.reset();
        filterRunning = true;
    }
    lowPassLog2Hz.setTarget(std::log2(t.lowPassHz));
    highPassLog2Hz.setTarget(std::log2(t.highPassHz));
    dryWeight.setTarget(t.dry);
    lowPassWeight.setTarget(t.lowPass);
    highPassWeight.setTarget(t.highPass);
}

void ChannelStrip::updateEqCoeffs() noexcept
{
    eqCoeffs[Low] = dsp::BiquadCoeffs::lowShelf(kEqLowHz, sampleRate, kEqQ, eqDb[Low].value());
    eqCoeffs[Mid] = dsp::BiquadCoeffs::peak(kEqMidHz, sampleRate, kEqQ, eqDb[Mid].value());
    eqCoeffs[High] = dsp::BiquadCoeffs::highShelf(kEqHighHz, sampleRate, kEqQ, eqDb[High].value());
}

double ChannelStrip::processEq(int ch, double x) noexcept
{
    const auto c = static_cast<std::size_t>(ch);
    for (std::size_t b = 0; b < kBands; ++b)
        x = eqState[b][c].process(eqCoeffs[b], x);
    return x;
}

double ChannelStrip::processIsolator(int ch, double x, double gLow, double gMid, double gHigh) noexcept
{
    const auto c = static_cast<std::size_t>(ch);
    double low = x, rest = x;
    for (std::size_t s = 0; s < 4; ++s)
    {
        low = lowSections[s][c].process(lp250[s % 2], low);
        rest = restSections[s][c].process(hp250[s % 2], rest);
    }
    for (std::size_t s = 0; s < 2; ++s)
        low = lowAllPass[s][c].process(ap2500[s], low); // the phase the mid/high split gives the other bands
    double mid = rest, high = rest;
    for (std::size_t s = 0; s < 4; ++s)
    {
        mid = midSections[s][c].process(lp2500[s % 2], mid);
        high = highSections[s][c].process(hp2500[s % 2], high);
    }
    return gLow * low + gMid * mid + gHigh * high;
}

void ChannelStrip::process(float* left, float* right, int numSamples) noexcept
{
    processPreFader(left, right, numSamples);
    applyFader(left, right, numSamples);
}

void ChannelStrip::applyFader(float* left, float* right, int numSamples) noexcept
{
    if (!faderGainRamp.isRamping() && faderGainRamp.value() == 1.0)
        return;
    for (int i = 0; i < numSamples; ++i)
    {
        const double g = faderGainRamp.next();
        left[i] = static_cast<float>(left[i] * g);
        right[i] = static_cast<float>(right[i] * g);
    }
}

void ChannelStrip::processPreFader(float* left, float* right, int numSamples) noexcept
{
    float* io[2] = {left, right};
    for (int start = 0; start < numSamples; start += kCoeffInterval)
    {
        const int n = std::min(kCoeffInterval, numSamples - start);

        // EQ mode coefficients follow the smoothed band gains once per sub-block.
        if (eqRunning && (eqDb[0].isRamping() || eqDb[1].isRamping() || eqDb[2].isRamping()))
        {
            for (auto& r : eqDb)
                r.skip(n);
            updateEqCoeffs();
        }

        for (int i = start; i < start + n; ++i)
        {
            const double trimG = trimGain.next();
            const double mix = modeMix.next();
            const double gLow = isoGain[Low].next(), gMid = isoGain[Mid].next(), gHigh = isoGain[High].next();

            double dry = 1.0, wl = 0.0, wh = 0.0;
            if (filterRunning)
            {
                dry = dryWeight.next();
                wl = lowPassWeight.next();
                wh = highPassWeight.next();
                const bool qMoving = filterQRamp.isRamping();
                const double q = filterQRamp.next();
                if (lowPassLog2Hz.isRamping() || qMoving)
                    lowPassCoeffs = dsp::SvfCoeffs::make(std::exp2(lowPassLog2Hz.next()), sampleRate, q);
                if (highPassLog2Hz.isRamping() || qMoving)
                    highPassCoeffs = dsp::SvfCoeffs::make(std::exp2(highPassLog2Hz.next()), sampleRate, q);
            }

            for (int ch = 0; ch < 2; ++ch)
            {
                const double x = static_cast<double>(io[ch][i]) * trimG;
                double y = 0.0;
                if (mix < 1.0)
                    y += (1.0 - mix) * processEq(ch, x);
                if (mix > 0.0)
                    y += mix * processIsolator(ch, x, gLow, gMid, gHigh);

                if (filterRunning)
                {
                    const auto c = static_cast<std::size_t>(ch);
                    const double lp = lowPassState[c].process(lowPassCoeffs, y).low;
                    const double hp = highPassState[c].process(highPassCoeffs, y).high;
                    y = dry * y + wl * lp + wh * hp;
                }
                io[ch][i] = static_cast<float>(y);
            }
        }

        // Stop running paths that are fully faded out.
        if (!modeMix.isRamping())
        {
            eqRunning = modeMix.value() < 1.0;
            isolatorRunning = modeMix.value() > 0.0;
        }
        if (filterRunning && !dryWeight.isRamping() && dryWeight.value() >= 1.0 && !lowPassLog2Hz.isRamping() &&
            !highPassLog2Hz.isRamping())
            filterRunning = false;
    }
}

} // namespace devildj::engine
