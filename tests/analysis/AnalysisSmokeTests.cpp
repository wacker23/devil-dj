// Smoke tests for src/analysis.

#include <catch2/catch_test_macros.hpp>

#include <complex>

TEST_CASE("std::complex<float> has the interleaved layout FFT code expects", "[smoke][analysis]")
{
    STATIC_CHECK(sizeof(std::complex<float>) == 2 * sizeof(float));

    const std::complex<float> z{3.0f, 4.0f};
    CHECK(std::abs(z) == 5.0f);
}
