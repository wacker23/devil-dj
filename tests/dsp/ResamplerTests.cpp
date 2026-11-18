#include "dsp/Resampler.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <vector>

using devildj::dsp::Resampler;

namespace
{
constexpr double kRate = 48000.0;

std::vector<float> sine(double hz, int n, double amp = 0.5)
{
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<std::size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * std::numbers::pi * hz * i / kRate));
    return x;
}

// Max error against the exact sine when reading at fractional positions with the given step.
double maxError(const Resampler& r, double hz, double step)
{
    const auto x = sine(hz, 48000);
    double worst = 0.0;
    for (double pos = 100.0; pos < 47000.0; pos += step)
    {
        const double exact = 0.5 * std::sin(2.0 * std::numbers::pi * hz * pos / kRate);
        worst = std::max(worst, std::abs(r.read(x.data(), static_cast<int>(x.size()), pos, step) - exact));
    }
    return worst;
}
} // namespace

TEST_CASE("Resampler reads integer positions exactly", "[dsp][rt-resampler]")
{
    Resampler r;
    r.prepare();
    const auto x = sine(1000.0, 1000);
    for (auto q : {Resampler::Quality::Hermite, Resampler::Quality::Sinc16})
    {
        r.setQuality(q);
        for (int i = 20; i < 980; i += 37)
        {
            const float v = r.read(x.data(), 1000, i, 1.0);
            if (q == Resampler::Quality::Hermite)
                CHECK(v == x[static_cast<std::size_t>(i)]);
            else
                CHECK(std::abs(v - x[static_cast<std::size_t>(i)]) < 1e-3f);
        }
    }
}

TEST_CASE("Resampler Hermite is accurate for musical frequencies", "[dsp][rt-resampler]")
{
    Resampler r;
    r.prepare();
    r.setQuality(Resampler::Quality::Hermite);
    CHECK(maxError(r, 1000.0, 1.08) < 1e-3); // -60 dB at 1 kHz (+8 % tempo)
    CHECK(maxError(r, 100.0, 0.94) < 1e-5);  // bass is near-perfect
}

TEST_CASE("Resampler Sinc16 is more accurate than Hermite at high frequencies", "[dsp][rt-resampler]")
{
    Resampler r;
    r.prepare();
    r.setQuality(Resampler::Quality::Hermite);
    const double hermite = maxError(r, 8000.0, 0.97);
    r.setQuality(Resampler::Quality::Sinc16);
    const double sinc = maxError(r, 8000.0, 0.97);
    INFO("8 kHz error: Hermite " << hermite << ", Sinc16 " << sinc);
    CHECK(sinc < hermite);
    CHECK(sinc < 5e-3);
}

TEST_CASE("Resampler treats samples outside the buffer as silence", "[dsp][rt-resampler]")
{
    Resampler r;
    r.prepare();
    const std::vector<float> ones(100, 1.0f);
    for (auto q : {Resampler::Quality::Hermite, Resampler::Quality::Sinc16})
    {
        r.setQuality(q);
        CHECK(r.read(ones.data(), 100, -50.0, 1.0) == 0.0f);
        CHECK(r.read(ones.data(), 100, 500.0, 1.0) == 0.0f);
        CHECK(std::abs(r.read(ones.data(), 100, 50.5, 1.0) - 1.0f) < 1e-3f); // DC passes in the middle
    }
}

TEST_CASE("Resampler works unprepared (falls back to Hermite)", "[dsp][rt-resampler]")
{
    Resampler r; // no prepare(): no tables
    r.setQuality(Resampler::Quality::Sinc16);
    const auto x = sine(440.0, 500);
    CHECK(r.read(x.data(), 500, 100.25, 1.0) == Resampler::readHermite(x.data(), 500, 100.25));
}
