// P1-10: channel strip DSP. Done when: isolator bands sum flat +-0.1 dB at 0 dB; kill attenuates > 60 dB at
// band centre; filter cutoff sweeps without clicks.

#include "dsp/Biquad.h"
#include "engine/AudioEngine.h"
#include "engine/ChannelStrip.h"
#include "util/RealtimeGuard.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>
#include <numbers>
#include <vector>

using namespace devildj::engine;
using devildj::dsp::BiquadCoeffs;
using devildj::dsp::BiquadState;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

ChannelStrip makeStrip(const std::function<void(ChannelStrip&)>& setup = {})
{
    ChannelStrip s;
    if (setup)
        setup(s);
    s.prepare(kRate, kBlock); // settings made before prepare are applied without ramps
    return s;
}

// Steady-state gain in dB at `hz`: run a sine until settled, then correlate whole periods.
double gainDb(ChannelStrip& strip, double hz)
{
    const int settle = static_cast<int>(0.3 * kRate);
    const int periods = std::max(4, static_cast<int>(0.2 * hz));
    const int measure = static_cast<int>(std::lround(periods * kRate / hz));
    const int total = settle + measure;
    std::vector<float> l(static_cast<std::size_t>(total)), r(l.size());
    const double w = 2.0 * std::numbers::pi * hz / kRate;
    for (int i = 0; i < total; ++i)
        l[static_cast<std::size_t>(i)] = r[static_cast<std::size_t>(i)] = static_cast<float>(0.5 * std::sin(w * i));
    for (int start = 0; start < total; start += kBlock)
        strip.process(l.data() + start, r.data() + start, std::min(kBlock, total - start));
    double re = 0.0, im = 0.0;
    for (int i = settle; i < total; ++i)
    {
        re += l[static_cast<std::size_t>(i)] * std::cos(w * i);
        im += l[static_cast<std::size_t>(i)] * std::sin(w * i);
    }
    const double amplitude = 2.0 * std::hypot(re, im) / measure;
    return 20.0 * std::log10(amplitude / 0.5);
}

std::vector<double> logFrequencies(double from, double to, int count)
{
    std::vector<double> f;
    for (int i = 0; i < count; ++i)
        f.push_back(from * std::pow(to / from, static_cast<double>(i) / (count - 1)));
    return f;
}

// Click detector: everything above 5 kHz in the output of a 100 Hz test tone is artefact (steps, zipper).
struct ClickMeter
{
    BiquadCoeffs hp[2] = {BiquadCoeffs::highPass(5000.0, kRate, devildj::dsp::kButterworth4Q[0]),
                          BiquadCoeffs::highPass(5000.0, kRate, devildj::dsp::kButterworth4Q[1])};
    BiquadState s[2];
    int skip = 2400; // the meter's own start-up
    double worst = 0.0;
    void feed(const float* x, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const double y = s[1].process(hp[1], s[0].process(hp[0], x[i]));
            if (skip > 0)
                --skip;
            else
                worst = std::max(worst, std::abs(y));
        }
    }
};

// A 20 ms linear ramp of a gain or cutoff on the tone leaves a small kink at each end of the ramp, which shows
// up around -45...-50 dB re the tone above 5 kHz (inaudible: it's a fade). A click (an unsmoothed step) is
// ~0 dB. The limit sits in between: -40 dB re the tone.
constexpr double kClickLimit = 0.25 * 0.01;

// Plays a 100 Hz tone (amplitude 0.25) through the strip block by block, calling `control(block)` before each.
double worstClick(ChannelStrip& strip, int blocks, const std::function<void(int)>& control)
{
    ClickMeter meter;
    float l[kBlock], r[kBlock];
    const double w = 2.0 * std::numbers::pi * 100.0 / kRate;
    for (int b = 0; b < blocks; ++b)
    {
        control(b);
        for (int i = 0; i < kBlock; ++i)
            l[i] = r[i] = static_cast<float>(0.25 * std::sin(w * (b * kBlock + i)));
        strip.process(l, r, kBlock);
        meter.feed(l, kBlock);
    }
    return meter.worst;
}
} // namespace

TEST_CASE("Isolator: all bands at 0 dB sum flat within 0.1 dB, 20 Hz - 20 kHz", "[engine][mixer]")
{
    double worst = 0.0, worstHz = 0.0;
    for (double hz : logFrequencies(20.0, 20000.0, 61))
    {
        auto s = makeStrip([](ChannelStrip& c) { c.setEqMode(ChannelStrip::EqMode::Isolator); });
        const double g = gainDb(s, hz);
        if (std::abs(g) > std::abs(worst))
        {
            worst = g;
            worstHz = hz;
        }
    }
    INFO("worst " << worst << " dB at " << worstHz << " Hz");
    CHECK(std::abs(worst) < 0.1);
}

TEST_CASE("Isolator: a kill takes more than 60 dB off at the band centre and leaves the others", "[engine][mixer]")
{
    struct Case
    {
        int band;
        double centreHz, otherHz;
    };
    // Band centres: geometric middle of 20-250 Hz, 250-2500 Hz, 2.5-20 kHz.
    for (const auto& c : {Case{ChannelStrip::Low, 70.7, 5000.0}, Case{ChannelStrip::Mid, 790.6, 60.0},
                          Case{ChannelStrip::High, 7071.0, 100.0}})
    {
        auto s = makeStrip(
            [&](ChannelStrip& strip)
            {
                strip.setEqMode(ChannelStrip::EqMode::Isolator);
                strip.setBandDb(c.band, ChannelStrip::kKillDb);
            });
        const double atCentre = gainDb(s, c.centreHz);
        auto s2 = makeStrip(
            [&](ChannelStrip& strip)
            {
                strip.setEqMode(ChannelStrip::EqMode::Isolator);
                strip.setBandDb(c.band, ChannelStrip::kKillDb);
            });
        const double elsewhere = gainDb(s2, c.otherHz);
        INFO("band " << c.band << ": " << atCentre << " dB at " << c.centreHz << " Hz, " << elsewhere << " dB at "
                     << c.otherHz << " Hz");
        CHECK(atCentre < -60.0);
        CHECK(std::abs(elsewhere) < 0.5);
    }
}

TEST_CASE("Isolator: +6 dB boosts its band by 6 dB", "[engine][mixer]")
{
    auto s = makeStrip(
        [](ChannelStrip& c)
        {
            c.setEqMode(ChannelStrip::EqMode::Isolator);
            c.setBandDb(ChannelStrip::Mid, 6.0);
        });
    CHECK(std::abs(gainDb(s, 790.6) - 6.0) < 0.1);
}

TEST_CASE("EQ mode: flat at 0 dB, shelves and peak where they should be", "[engine][mixer]")
{
    for (double hz : logFrequencies(20.0, 20000.0, 13))
    {
        auto flat = makeStrip();
        CHECK(std::abs(gainDb(flat, hz)) < 0.01);
    }
    auto low = makeStrip([](ChannelStrip& c) { c.setBandDb(ChannelStrip::Low, -26.0); });
    const double lowAt20 = gainDb(low, 20.0);
    auto low2 = makeStrip([](ChannelStrip& c) { c.setBandDb(ChannelStrip::Low, -26.0); });
    const double lowAt2k = gainDb(low2, 2000.0);
    auto mid = makeStrip([](ChannelStrip& c) { c.setBandDb(ChannelStrip::Mid, 6.0); });
    const double midAt1k = gainDb(mid, 1000.0);
    auto high = makeStrip([](ChannelStrip& c) { c.setBandDb(ChannelStrip::High, -26.0); });
    const double highAt20k = gainDb(high, 20000.0);
    auto high2 = makeStrip([](ChannelStrip& c) { c.setBandDb(ChannelStrip::High, -26.0); });
    const double highAt500 = gainDb(high2, 500.0);
    INFO("low shelf -26: " << lowAt20 << " dB @20 Hz, " << lowAt2k << " @2 kHz; peak +6: " << midAt1k
                           << "; high shelf -26: " << highAt20k << " @20 kHz, " << highAt500 << " @500 Hz");
    CHECK(lowAt20 < -22.0);
    CHECK(std::abs(lowAt2k) < 0.5);
    CHECK(std::abs(midAt1k - 6.0) < 0.05);
    CHECK(highAt20k < -18.0);
    CHECK(std::abs(highAt500) < 0.5);
    // In EQ mode a kill is the bottom of the range.
    auto killed = makeStrip([](ChannelStrip& c) { c.setBandDb(ChannelStrip::Mid, ChannelStrip::kKillDb); });
    CHECK(std::abs(gainDb(killed, 1000.0) + 26.0) < 0.05);
}

TEST_CASE("Filter: centre is off; the knob maps to the documented cutoffs", "[engine][mixer]")
{
    const auto off = ChannelStrip::filterTarget(0.5);
    CHECK(off.dry == 1.0);
    const auto deadZone = ChannelStrip::filterTarget(0.515);
    CHECK(deadZone.dry == 1.0);
    const auto lpEnd = ChannelStrip::filterTarget(0.0);
    CHECK(std::abs(lpEnd.lowPassHz - 60.0) < 1e-6);
    CHECK(lpEnd.lowPass == 1.0);
    const auto lpStart = ChannelStrip::filterTarget(0.479999);
    CHECK(lpStart.lowPassHz > 19900.0);
    const auto hpEnd = ChannelStrip::filterTarget(1.0);
    CHECK(std::abs(hpEnd.highPassHz - 10000.0) < 1e-6);
    CHECK(hpEnd.highPass == 1.0);

    auto centre = makeStrip();
    CHECK(std::abs(gainDb(centre, 1000.0)) < 1e-6);
    auto lowPass = makeStrip([](ChannelStrip& c) { c.setFilter(0.0); });
    const double lp = gainDb(lowPass, 2000.0);
    auto highPass = makeStrip([](ChannelStrip& c) { c.setFilter(1.0); });
    const double hp = gainDb(highPass, 200.0);
    auto halfLp = makeStrip([](ChannelStrip& c) { c.setFilter(0.24); }); // ~1.1 kHz
    const double pass = gainDb(halfLp, 100.0);
    INFO("LP 60 Hz @2 kHz " << lp << " dB, HP 10 kHz @200 Hz " << hp << " dB, LP ~1.1 kHz @100 Hz " << pass);
    CHECK(lp < -50.0);
    CHECK(hp < -60.0);
    CHECK(std::abs(pass) < 0.5);
}

TEST_CASE("Filter: sweeping and jumping the knob doesn't click", "[engine][mixer]")
{
    auto s = makeStrip();
    constexpr int blocks = static_cast<int>(6.0 * kRate / kBlock);
    const double worst = worstClick(s, blocks,
                                    [&](int b)
                                    {
                                        const double t = b * kBlock / kRate;
                                        if (t < 2.0) // slow sweep full left to full right
                                            s.setFilter(t / 2.0);
                                        else if (t < 4.0) // back, faster, with resonance up
                                        {
                                            s.setFilterResonance(0.8);
                                            s.setFilter(1.0 - (t - 2.0) / 0.5 * 0.25 * 1.0);
                                        }
                                        else // jumps every 100 ms (smoothed over 20 ms)
                                        {
                                            const int step = static_cast<int>((t - 4.0) / 0.1);
                                            s.setFilter(step % 3 == 0 ? 0.1 : step % 3 == 1 ? 0.9 : 0.5);
                                        }
                                    });
    INFO("worst > 5 kHz: " << worst << " (" << 20.0 * std::log10(worst / 0.25) << " dB re the tone)");
    CHECK(worst < kClickLimit);
}

TEST_CASE("EQ: kills, boosts and mode switches don't click", "[engine][mixer]")
{
    auto s = makeStrip();
    const double worst =
        worstClick(s, 400,
                   [&](int b)
                   {
                       if (b % 40 == 0)
                           s.setBandDb(ChannelStrip::Low, (b / 40) % 2 ? ChannelStrip::kKillDb : 6.0);
                       if (b % 100 == 50)
                           s.setEqMode((b / 100) % 2 ? ChannelStrip::EqMode::Eq : ChannelStrip::EqMode::Isolator);
                       if (b % 30 == 15)
                           s.setTrimDb((b / 30) % 2 ? 12.0 : -12.0);
                   });
    INFO("worst > 5 kHz: " << worst);
    CHECK(worst < kClickLimit);
}

TEST_CASE("Parameters move over 20 ms", "[engine][mixer]")
{
    auto s = makeStrip();
    s.setTrimDb(6.0);
    std::vector<float> l(2400, 1.0f), r(2400, 1.0f);
    s.process(l.data(), r.data(), 2400);
    CHECK(l[0] < 1.01f);
    CHECK(std::abs(l[479] - 1.5f) < 0.01f);     // half way at 10 ms
    CHECK(std::abs(l[959] - 1.99526f) < 1e-3f); // there at 20 ms (+6 dB)
    CHECK(std::abs(l[2399] - 1.99526f) < 1e-4f);
}

TEST_CASE("Fader curves and trim", "[engine][mixer]")
{
    using C = ChannelStrip::FaderCurve;
    CHECK(ChannelStrip::faderGain(0.0, C::Smooth) == 0.0);
    CHECK(ChannelStrip::faderGain(0.5, C::Smooth) == 0.25);
    CHECK(ChannelStrip::faderGain(1.0, C::Smooth) == 1.0);
    CHECK(ChannelStrip::faderGain(0.5, C::Linear) == 0.5);
    CHECK(ChannelStrip::faderGain(0.5, C::Sharp) == 0.875);
    CHECK(ChannelStrip::faderGain(1.0, C::Sharp) == 1.0);
    CHECK(ChannelStrip::faderGain(0.0, C::Sharp) == 0.0);

    auto s = makeStrip(
        [](ChannelStrip& c)
        {
            c.setTrimDb(20.0); // clamps to +12
            c.setFader(0.5);
            c.setFaderCurve(C::Linear);
        });
    CHECK(s.trimDb() == 12.0);
    CHECK(std::abs(gainDb(s, 1000.0) - (12.0 - 6.0206)) < 0.01);
}

TEST_CASE("Engine: SetParam drives the channel strip; no allocation", "[engine][mixer]")
{
    auto t = std::make_unique<TrackBuffer>();
    t->sampleRate = kRate;
    t->audio.setSize(2, static_cast<int>(kRate));
    for (int i = 0; i < t->audio.getNumSamples(); ++i)
        for (int ch = 0; ch < 2; ++ch)
            t->audio.setSample(ch, i, 0.5f);
    AudioEngine e;
    e.prepare(kRate, kBlock);
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[] = {l.data(), r.data()};
    e.send(LoadTrack{0, t.get(), 1});
    e.send(Play{0});
    for (int i = 0; i < 10; ++i)
        e.process(outs, 2, kBlock);
    const float before = l[kBlock - 1];

    const auto violations = devildj::util::realtimeViolationCount();
    e.send(SetParam{channelParam(0, ChannelParam::Fader), 0.5f});
    e.send(SetParam{channelParam(0, ChannelParam::EqMode), 1.0f});
    e.send(SetParam{channelParam(0, ChannelParam::EqLow), -1000.0f});
    e.send(SetParam{channelParam(0, ChannelParam::Filter), 0.3f});
    e.send(SetParam{channelParam(1, ChannelParam::Trim), 3.0f});
    for (int i = 0; i < 20; ++i)
        e.process(outs, 2, kBlock);
    const auto& c = e.readState().channels;
    CHECK(c[0].fader == 0.5f);
    CHECK(c[0].eqMode == 1);
    CHECK(std::isinf(c[0].eqDb[0]));
    CHECK(c[0].filter == 0.3f);
    CHECK(c[1].trimDb == 3.0f);
    CHECK(std::abs(l[kBlock - 1]) < 0.05f * std::abs(before)); // DC killed by the low band
    if (devildj::util::kRealtimeGuardEnabled)
        CHECK(devildj::util::realtimeViolationCount() == violations);
}

TEST_CASE("Click meter: a hard step reads far above the limit (control)", "[engine][mixer]")
{
    ClickMeter meter;
    std::vector<float> x(9600);
    const double w = 2.0 * std::numbers::pi * 100.0 / kRate;
    for (int i = 0; i < 9600; ++i)
        x[static_cast<std::size_t>(i)] = static_cast<float>((i < 4800 ? 0.25 : 0.125) * std::sin(w * i + 1.0));
    meter.feed(x.data(), 9600);
    INFO("step reads " << meter.worst);
    CHECK(meter.worst > 10.0 * kClickLimit);
}
