#include "analysis/WaveformBuilder.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <vector>

using devildj::analysis::buildWaveform;
using devildj::analysis::Waveform;

namespace
{
constexpr double kRate = 48000.0;

Waveform sineWaveform(double hz, double seconds = 2.0)
{
    const int n = static_cast<int>(seconds * kRate);
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] = static_cast<float>(0.8 * std::sin(2.0 * std::numbers::pi * hz * i / kRate));
    return buildWaveform(x.data(), x.data(), n, kRate);
}

float steadyPeak(const Waveform& w, int band, bool rms = false)
{
    const auto& v = (rms ? w.rms : w.peak)[static_cast<std::size_t>(band)];
    float p = 0.0f;
    for (std::size_t i = v.size() / 2; i < v.size(); ++i)
        p = std::max(p, v[i]);
    return p;
}
} // namespace

TEST_CASE("Waveform: 150 points per second, overview of 1,024 points", "[analysis][waveform]")
{
    const auto w = sineWaveform(1000.0, 10.0);
    CHECK(w.samplesPerPoint == 320);
    CHECK(w.pointsPerSecond() == 150.0);
    CHECK(w.numPoints() == 1500);
    for (int b = 0; b < Waveform::kBands; ++b)
        CHECK(w.overviewPeak[static_cast<std::size_t>(b)].size() == 1024);
    CHECK(w.overviewTotalPeak.size() == 1024);

    const auto at44 = buildWaveform(nullptr, nullptr, 0, 44100.0);
    CHECK(at44.samplesPerPoint == 294);
    CHECK(at44.empty());
}

TEST_CASE("Waveform: tones land in the right band", "[analysis][waveform]")
{
    using W = Waveform;
    const auto bass = sineWaveform(60.0);
    CHECK(steadyPeak(bass, W::Low) == Catch::Approx(0.8f).epsilon(0.03));
    CHECK(steadyPeak(bass, W::Mid) < 0.05f);
    CHECK(steadyPeak(bass, W::High) < 0.01f);

    const auto mid = sineWaveform(800.0);
    CHECK(steadyPeak(mid, W::Mid) > 0.7f);
    CHECK(steadyPeak(mid, W::Low) < 0.1f);
    CHECK(steadyPeak(mid, W::High) < 0.1f);

    const auto high = sineWaveform(8000.0);
    // At 8 kHz there are only 6 samples per cycle, so sample peaks miss the crest; RMS doesn't care.
    CHECK(steadyPeak(high, W::High, true) == Catch::Approx(0.8f / std::sqrt(2.0f)).epsilon(0.03));
    CHECK(steadyPeak(high, W::High) > 0.65f);
    CHECK(steadyPeak(high, W::Mid) < 0.05f);
    CHECK(steadyPeak(high, W::Low) < 0.001f);
}

TEST_CASE("Waveform: a click shows up at its own point (alignment)", "[analysis][waveform]")
{
    const int n = static_cast<int>(4.0 * kRate);
    std::vector<float> x(static_cast<std::size_t>(n), 0.0f);
    const int clicks[] = {12000, 48000, 100001, 150400};
    for (int c : clicks)
        for (int i = 0; i < 48; ++i) // 1 ms click burst
            x[static_cast<std::size_t>(c + i)] = (i % 2 == 0 ? 0.9f : -0.9f);
    const auto w = buildWaveform(x.data(), x.data(), n, kRate);
    const auto& high = w.peak[Waveform::High];
    for (int c : clicks)
    {
        const int expected = c / w.samplesPerPoint;
        int loudest = expected - 5;
        for (int p = expected - 5; p <= expected + 5; ++p)
            if (high[static_cast<std::size_t>(p)] > high[static_cast<std::size_t>(loudest)])
                loudest = p;
        INFO("click at sample " << c);
        CHECK(std::abs(loudest - expected) <= 1); // within one point (6.7 ms)
    }
}

TEST_CASE("Waveform: building a 10-minute track is quick", "[analysis][waveform]")
{
    const int n = static_cast<int>(600.0 * kRate);
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] = static_cast<float>(0.5 * std::sin(i * 0.01));
    const auto start = std::chrono::steady_clock::now();
    const auto w = buildWaveform(x.data(), x.data(), n, kRate);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    WARN("10-minute waveform built in " << seconds << " s");
    CHECK(w.numPoints() == 90000);
#if defined(NDEBUG)
    CHECK(seconds < 2.0);
#endif
}
