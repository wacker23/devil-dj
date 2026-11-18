#include "analysis/WaveformOverview.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <vector>

using devildj::analysis::buildOverview;

TEST_CASE("Overview: peak and RMS per point", "[analysis][waveform]")
{
    // First half silent, second half a full-scale sine (left) and half-scale (right).
    const int n = 48000;
    std::vector<float> l(static_cast<std::size_t>(n), 0.0f), r(l.size(), 0.0f);
    for (int i = n / 2; i < n; ++i)
    {
        l[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 * i / 48000.0));
        r[static_cast<std::size_t>(i)] = 0.5f * l[static_cast<std::size_t>(i)];
    }
    const auto o = buildOverview(l.data(), r.data(), n, 100);
    REQUIRE(o.size() == 100);
    CHECK(o.peak[10] == 0.0f);
    CHECK(o.rms[10] == 0.0f);
    CHECK(o.peak[80] == Catch::Approx(1.0f).margin(1e-3));
    // RMS of the channel pair: sqrt((0.5 + 0.125) / 2)
    CHECK(o.rms[80] == Catch::Approx(std::sqrt((0.5 + 0.125) / 2.0)).epsilon(0.01));
}

TEST_CASE("Overview: odd lengths, short tracks and empty input", "[analysis][waveform]")
{
    std::vector<float> x(1001, 0.25f);
    const auto o = buildOverview(x.data(), x.data(), 1001, 1024); // fewer samples than points
    REQUIRE(o.size() == 1024);
    int filled = 0;
    for (float p : o.peak)
        filled += p == 0.25f ? 1 : 0;
    CHECK(filled == 1001); // every sample in exactly one point; the rest are empty
    const auto empty = buildOverview(nullptr, nullptr, 0, 16);
    CHECK(empty.size() == 16);
    CHECK(empty.peak[0] == 0.0f);
}
