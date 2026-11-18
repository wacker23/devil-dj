#include "dsp/SincResampler.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <numbers>
#include <vector>

using devildj::dsp::resample;
using devildj::dsp::resampledLength;

namespace
{
std::vector<float> sine(double hz, double rate, double seconds, double amplitude = 0.5, double phase = 0.0)
{
    std::vector<float> x(static_cast<std::size_t>(seconds * rate));
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(amplitude *
                                  std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate + phase));
    return x;
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (auto i = from; i < to; ++i)
        sum += static_cast<double>(x[i]) * x[i];
    return std::sqrt(sum / static_cast<double>(to - from));
}

// Skip the edges, where the kernel runs past the start/end of the signal.
constexpr double kEdge = 0.05;
} // namespace

TEST_CASE("resampledLength rounds to the nearest sample", "[dsp][resampler]")
{
    CHECK(resampledLength(44100, 44100.0, 48000.0) == 48000);
    CHECK(resampledLength(48000, 48000.0, 44100.0) == 44100);
    CHECK(resampledLength(1, 44100.0, 48000.0) == 1);
    CHECK(resampledLength(441, 44100.0, 48000.0) == 480);
    CHECK(resampledLength(100, 0.0, 48000.0) == 0);
}

TEST_CASE("Equal rates return an identical copy", "[dsp][resampler]")
{
    const auto x = sine(440.0, 48000.0, 0.1);
    CHECK(resample(x, 48000.0, 48000.0) == x);
    CHECK(resample({}, 44100.0, 48000.0).empty());
}

TEST_CASE("44.1 -> 48 kHz: a 1 kHz sine comes out as the ideal 1 kHz sine at 48 kHz", "[dsp][resampler]")
{
    const auto in = sine(1000.0, 44100.0, 1.0);
    const auto out = resample(in, 44100.0, 48000.0);
    const auto ideal = sine(1000.0, 48000.0, 1.0);
    REQUIRE(out.size() == ideal.size());

    double maxErr = 0.0;
    for (auto i = static_cast<std::size_t>(kEdge * 48000); i < out.size() - static_cast<std::size_t>(kEdge * 48000);
         ++i)
        maxErr = std::max(maxErr, std::abs(static_cast<double>(out[i]) - ideal[i]));
    CHECK(maxErr < 1e-4); // better than -80 dB relative to full scale
}

TEST_CASE("48 -> 44.1 kHz preserves the passband level", "[dsp][resampler]")
{
    for (double hz : {100.0, 1000.0, 10000.0, 19000.0})
    {
        const auto out = resample(sine(hz, 48000.0, 0.5), 48000.0, 44100.0);
        const auto from = static_cast<std::size_t>(kEdge * 44100), to = out.size() - from;
        const double levelDb = 20.0 * std::log10(rms(out, from, to) / (0.5 / std::sqrt(2.0)));
        INFO(hz << " Hz: " << levelDb << " dB");
        CHECK(std::abs(levelDb) < 0.05);
    }
}

TEST_CASE("Downsampling removes content above the new Nyquist (no aliasing)", "[dsp][resampler]")
{
    // 30 kHz at 96 kHz would fold to 18 kHz at 48 kHz without the low-pass.
    const auto out = resample(sine(30000.0, 96000.0, 0.5), 96000.0, 48000.0);
    const auto from = static_cast<std::size_t>(kEdge * 48000), to = out.size() - from;
    const double levelDb = 20.0 * std::log10(rms(out, from, to) / (0.5 / std::sqrt(2.0)) + 1e-12);
    INFO("alias level " << levelDb << " dB");
    CHECK(levelDb < -80.0);
}

TEST_CASE("DC passes with unity gain", "[dsp][resampler]")
{
    const std::vector<float> ones(44100, 1.0f);
    for (double outRate : {48000.0, 22050.0, 96000.0})
    {
        const auto out = resample(ones, 44100.0, outRate);
        const auto mid = out[out.size() / 2];
        INFO(outRate);
        CHECK(std::abs(mid - 1.0f) < 1e-4f);
    }
}

TEST_CASE("Resampling keeps timing: an impulse at t lands at t", "[dsp][resampler]")
{
    std::vector<float> in(44100, 0.0f);
    in[22050] = 1.0f; // t = 0.5 s
    const auto out = resample(in, 44100.0, 48000.0);
    const auto peak = std::max_element(out.begin(), out.end()) - out.begin();
    CHECK(std::abs(peak - 24000) <= 1);
}

TEST_CASE("Resampler speed: 60 s at 44.1 kHz to 48 kHz", "[dsp][resampler][.perf]")
{
    const auto in = sine(1000.0, 44100.0, 60.0);
    const auto start = std::chrono::steady_clock::now();
    const auto out = resample(in, 44100.0, 48000.0);
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    WARN("60 s mono, 44.1 -> 48 kHz: " << seconds << " s");
    CHECK(out.size() == 2'880'000);
}
