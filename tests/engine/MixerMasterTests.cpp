// P1-11: crossfader, master gain, limiter, meters. Done when: unit tests for curve values at 0/0.5/1; limiter
// never exceeds the ceiling on a +12 dB test signal.

#include "dsp/LevelMeter.h"
#include "dsp/Limiter.h"
#include "engine/AudioEngine.h"
#include "engine/Crossfader.h"
#include "util/RealtimeGuard.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace devildj::engine;
using Catch::Approx;
using devildj::dsp::LevelMeter;
using devildj::dsp::Limiter;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

double gainA(double x, CrossfaderCurve c)
{
    return crossfaderGain(CrossfaderAssign::A, x, c);
}
double gainB(double x, CrossfaderCurve c)
{
    return crossfaderGain(CrossfaderAssign::B, x, c);
}

// Runs a signal through a limiter in blocks; returns the output.
std::vector<float> limit(Limiter& lim, std::vector<float> l, std::vector<float>& r, int block = kBlock)
{
    for (std::size_t start = 0; start < l.size(); start += static_cast<std::size_t>(block))
    {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), l.size() - start));
        lim.process(l.data() + start, r.data() + start, n);
    }
    return l;
}
} // namespace

TEST_CASE("Crossfader curves at 0, 0.5 and 1", "[engine][mixer][crossfader]")
{
    using C = CrossfaderCurve;
    // Smooth (constant power)
    CHECK(gainA(0.0, C::Smooth) == 1.0);
    CHECK(gainB(0.0, C::Smooth) == 0.0);
    CHECK(gainA(0.5, C::Smooth) == Approx(std::sqrt(0.5)).margin(1e-12));
    CHECK(gainB(0.5, C::Smooth) == Approx(std::sqrt(0.5)).margin(1e-12));
    CHECK(gainA(1.0, C::Smooth) == 0.0);
    CHECK(gainB(1.0, C::Smooth) == 1.0);
    // constant power everywhere
    for (double x = 0.0; x <= 1.0; x += 0.05)
        CHECK(gainA(x, C::Smooth) * gainA(x, C::Smooth) + gainB(x, C::Smooth) * gainB(x, C::Smooth) ==
              Approx(1.0).margin(1e-12));
    // Linear
    CHECK(gainA(0.0, C::Linear) == 1.0);
    CHECK(gainB(0.0, C::Linear) == 0.0);
    CHECK(gainA(0.5, C::Linear) == 0.5);
    CHECK(gainB(0.5, C::Linear) == 0.5);
    CHECK(gainA(1.0, C::Linear) == 0.0);
    CHECK(gainB(1.0, C::Linear) == 1.0);
    // Cut: full level until 5 % from the far end
    CHECK(gainA(0.0, C::Cut) == 1.0);
    CHECK(gainB(0.0, C::Cut) == 0.0);
    CHECK(gainA(0.5, C::Cut) == 1.0);
    CHECK(gainB(0.5, C::Cut) == 1.0);
    CHECK(gainA(1.0, C::Cut) == 0.0);
    CHECK(gainB(1.0, C::Cut) == 1.0);
    CHECK(gainA(0.95, C::Cut) == 1.0);
    CHECK(gainA(0.975, C::Cut) == Approx(0.5));
    CHECK(gainB(0.025, C::Cut) == Approx(0.5));
    CHECK(gainB(0.05, C::Cut) == 1.0);
    // THRU ignores the crossfader
    for (double x : {0.0, 0.5, 1.0})
        CHECK(crossfaderGain(CrossfaderAssign::Thru, x, C::Cut) == 1.0);
}

TEST_CASE("Limiter never exceeds the ceiling on +12 dB signals", "[engine][mixer][limiter]")
{
    Limiter lim;
    lim.prepare(kRate, kBlock);
    const float ceiling = lim.ceiling();
    CHECK(ceiling == Approx(std::pow(10.0, -0.3 / 20.0)).margin(1e-6));
    CHECK(lim.latencySamples() == 72); // 1.5 ms at 48 kHz

    const int n = static_cast<int>(3.0 * kRate);
    std::mt19937 rng(3);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    struct Signal
    {
        const char* name;
        std::vector<float> l, r;
    };
    std::vector<Signal> signals;
    {
        Signal s{"sine 100 Hz +12 dB", std::vector<float>(static_cast<std::size_t>(n)),
                 std::vector<float>(static_cast<std::size_t>(n))};
        for (int i = 0; i < n; ++i)
            s.l[static_cast<std::size_t>(i)] = s.r[static_cast<std::size_t>(i)] =
                static_cast<float>(3.98 * std::sin(2.0 * std::numbers::pi * 100.0 * i / kRate));
        signals.push_back(std::move(s));
    }
    {
        Signal s{"noise, peaks ~+12 dB", std::vector<float>(static_cast<std::size_t>(n)),
                 std::vector<float>(static_cast<std::size_t>(n))};
        for (int i = 0; i < n; ++i)
        {
            s.l[static_cast<std::size_t>(i)] = 1.3f * noise(rng);
            s.r[static_cast<std::size_t>(i)] = 1.3f * noise(rng);
        }
        signals.push_back(std::move(s));
    }
    {
        // Silence with isolated +12 dB clicks (worst case for look-ahead), and a kick-like burst.
        Signal s{"clicks and bursts", std::vector<float>(static_cast<std::size_t>(n)),
                 std::vector<float>(static_cast<std::size_t>(n))};
        for (int i = 1000; i < n; i += 4801)
        {
            s.l[static_cast<std::size_t>(i)] = 3.98f;
            s.r[static_cast<std::size_t>(i) + 1] = -3.98f;
        }
        for (int i = 0; i < 4800; ++i)
            s.l[static_cast<std::size_t>(60000 + i)] =
                static_cast<float>(3.98 * std::exp(-i / 1200.0) * std::sin(2.0 * std::numbers::pi * 55.0 * i / kRate));
        signals.push_back(std::move(s));
    }

    for (auto& s : signals)
    {
        for (int block : {kBlock, 1, 37})
        {
            lim.reset();
            auto r = s.r;
            const auto l = limit(lim, s.l, r, block);
            float worst = 0.0f;
            for (std::size_t i = 0; i < l.size(); ++i)
                worst = std::max({worst, std::abs(l[i]), std::abs(r[i])});
            INFO(s.name << ", blocks of " << block << ": peak " << worst << " (ceiling " << ceiling << ")");
            CHECK(worst <= ceiling);
        }
    }
}

TEST_CASE("Limiter: material under the ceiling passes untouched (just delayed)", "[engine][mixer][limiter]")
{
    Limiter lim;
    lim.prepare(kRate, kBlock);
    const int n = 9600;
    std::vector<float> l(static_cast<std::size_t>(n)), r(l.size());
    for (int i = 0; i < n; ++i)
        l[static_cast<std::size_t>(i)] = r[static_cast<std::size_t>(i)] =
            static_cast<float>(0.9 * std::sin(2.0 * std::numbers::pi * 440.0 * i / kRate));
    const auto in = l;
    const auto out = limit(lim, l, r);
    const auto d = static_cast<std::size_t>(lim.latencySamples());
    for (std::size_t i = d; i < out.size(); ++i)
        REQUIRE(out[i] == in[i - d]);
    CHECK(lim.lastGainReductionDb() == 0.0f);
}

TEST_CASE("Limiter: +12 dB sine is brought down by ~12.3 dB with a smooth gain", "[engine][mixer][limiter]")
{
    Limiter lim;
    lim.prepare(kRate, kBlock);
    const int n = 48000;
    std::vector<float> l(static_cast<std::size_t>(n)), r(l.size());
    for (int i = 0; i < n; ++i)
        l[static_cast<std::size_t>(i)] = r[static_cast<std::size_t>(i)] =
            static_cast<float>(3.98 * std::sin(2.0 * std::numbers::pi * 1000.0 * i / kRate));
    const auto out = limit(lim, l, r);
    CHECK(lim.lastGainReductionDb() == Approx(20.0 * std::log10(3.98 / lim.ceiling())).margin(0.2));
    float peak = 0.0f;
    for (std::size_t i = 24000; i < out.size(); ++i)
        peak = std::max(peak, std::abs(out[i]));
    CHECK(peak > 0.9f * lim.ceiling()); // it limits, it doesn't just mute
}

TEST_CASE("Level meter: peak, RMS and peak fall", "[engine][mixer][meter]")
{
    LevelMeter m;
    m.prepare(kRate);
    std::vector<float> l(static_cast<std::size_t>(kBlock) * 750), r(l.size()); // 4 s in whole blocks (RMS settles)
    for (std::size_t i = 0; i < l.size(); ++i)
    {
        l[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kRate));
        r[i] = 0.5f * l[i];
    }
    for (std::size_t s = 0; s < l.size(); s += kBlock)
        m.process(l.data() + s, r.data() + s, kBlock);
    CHECK(m.peakLeft() == Approx(0.5f).margin(1e-3));
    CHECK(m.peakRight() == Approx(0.25f).margin(1e-3));
    // RMS of the two channels together: sqrt((0.5^2/2 + 0.25^2/2) / 2)
    CHECK(m.rms() == Approx(std::sqrt((0.125 + 0.03125) / 2.0)).epsilon(0.01));
    m.processSilence(static_cast<int>(kRate)); // one second later: -20 dB
    CHECK(m.peakLeft() == Approx(0.05f).epsilon(0.01));
    // RMS (300 ms on the mean square) after 1 s of silence: x exp(-1 / 0.6) in amplitude.
    CHECK(m.rms() == Approx(std::sqrt((0.125 + 0.03125) / 2.0) * std::exp(-1.0 / 0.6)).epsilon(0.02));
}

TEST_CASE("Engine: crossfader, assignment, master gain, limiter and meters", "[engine][mixer]")
{
    auto track = [](float v)
    {
        auto t = std::make_unique<TrackBuffer>();
        t->sampleRate = kRate;
        t->audio.setSize(2, static_cast<int>(kRate * 4));
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < t->audio.getNumSamples(); ++i)
                t->audio.setSample(ch, i, v);
        return t;
    };
    const auto a = track(0.25f), b = track(0.125f);
    AudioEngine e;
    e.prepare(kRate, kBlock);
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[] = {l.data(), r.data()};
    const auto run = [&](int blocks)
    {
        for (int i = 0; i < blocks; ++i)
            e.process(outs, 2, kBlock);
    };
    e.send(LoadTrack{0, a.get(), 1});
    e.send(LoadTrack{1, b.get(), 2});
    e.send(Play{0});
    e.send(Play{1});
    run(4);
    const auto& st = e.readState();
    CHECK(st.channels[0].crossfaderAssign == 0); // A
    CHECK(st.channels[1].crossfaderAssign == 2); // B
    CHECK(st.crossfader == 0.5f);
    CHECK(st.masterLatencySamples == 72);

    const auto violations = devildj::util::realtimeViolationCount();
    e.send(SetParam{ParamId::Crossfader, 0.0f}); // full A
    run(10);
    CHECK(l.back() == 0.25f);
    e.send(SetParam{ParamId::Crossfader, 1.0f}); // full B
    run(10);
    CHECK(l.back() == 0.125f);
    e.send(SetParam{channelParam(0, ChannelParam::CrossfaderAssign), 1.0f}); // A to THRU
    run(10);
    CHECK(l.back() == 0.375f);
    e.send(SetParam{ParamId::CrossfaderCurve, 2.0f}); // cut
    e.send(SetParam{ParamId::Crossfader, 0.5f});
    run(10);
    CHECK(l.back() == 0.375f);
    CHECK(e.readState().crossfaderCurve == 2);

    e.send(SetParam{ParamId::MasterGain, -6.0f});
    run(200); // 1 s: the meter's peak has fallen to the new level, RMS has settled
    CHECK(l.back() == Approx(0.375f * std::pow(10.0, -6.0 / 20.0)).margin(1e-6));
    CHECK(e.readState().masterPeakL == Approx(0.375f * std::pow(10.0, -6.0 / 20.0)).margin(0.002));
    CHECK(e.readState().masterRms == Approx(0.375f * std::pow(10.0, -6.0 / 20.0)).margin(0.002));

    // Push it into the limiter: +12 dB trim on both channels and master +6 dB.
    e.send(SetParam{ParamId::MasterGain, 6.0f});
    e.send(SetParam{channelParam(0, ChannelParam::Trim), 12.0f});
    e.send(SetParam{channelParam(1, ChannelParam::Trim), 12.0f});
    float worst = 0.0f;
    for (int i = 0; i < 40; ++i)
    {
        run(1);
        for (int s = 0; s < kBlock; ++s)
            worst = std::max(worst, std::abs(l[static_cast<std::size_t>(s)]));
    }
    CHECK(worst <= static_cast<float>(std::pow(10.0, -0.3 / 20.0)));
    // 0.25 and 0.125 at +12 dB trim = 1.5, master +6 dB = 3.0: (exactly 2.979 with +12 dB = x3.98 and +6 dB = x1.995)
    // needs 9.78 dB.
    CHECK(e.readState().limiterGainReductionDb == Approx(9.78f).margin(0.02));
    CHECK(e.readState().channels[0].peakL == Approx(1.0f).margin(0.01)); // pre-fader, after +12 dB trim
    if (devildj::util::kRealtimeGuardEnabled)
        CHECK(devildj::util::realtimeViolationCount() == violations);
}

TEST_CASE("Engine: moving the crossfader is smoothed, not a step", "[engine][mixer][crossfader]")
{
    auto t = std::make_unique<TrackBuffer>();
    t->sampleRate = kRate;
    t->audio.setSize(2, static_cast<int>(kRate));
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < t->audio.getNumSamples(); ++i)
            t->audio.setSample(ch, i, 0.5f);
    AudioEngine e;
    e.prepare(kRate, kBlock);
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[] = {l.data(), r.data()};
    e.send(LoadTrack{0, t.get(), 1});
    e.send(Play{0});
    e.send(SetParam{ParamId::Crossfader, 0.0f});
    for (int i = 0; i < 10; ++i)
        e.process(outs, 2, kBlock);
    e.send(SetParam{ParamId::Crossfader, 1.0f}); // slam it to B: deck A fades out over 20 ms
    float maxStep = 0.0f, previous = l.back();
    for (int i = 0; i < 8; ++i)
    {
        e.process(outs, 2, kBlock);
        for (float v : l)
        {
            maxStep = std::max(maxStep, std::abs(v - previous));
            previous = v;
        }
    }
    CHECK(l.back() == 0.0f);
    CHECK(maxStep < 0.5f / 900.0f); // 0.5 over ~960 samples
}
