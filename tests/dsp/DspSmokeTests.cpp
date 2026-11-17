// Smoke tests for src/dsp.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <numbers>

TEST_CASE("Atomic float parameters are lock-free", "[smoke][dsp]")
{
    // Smoothed parameters may be read on the audio thread from atomics; they must never lock.
    STATIC_CHECK(std::atomic<float>::is_always_lock_free);
    STATIC_CHECK(std::atomic<double>::is_always_lock_free);
}

TEST_CASE("Equal-power crossfade gains sum to unit power", "[smoke][dsp]")
{
    // Same curve as the smooth crossfader (ARCHITECTURE §4); a sanity check that <numbers> and <cmath> work.
    for (int i = 0; i <= 10; ++i)
    {
        const double x = i / 10.0;
        const double gA = std::cos(x * std::numbers::pi / 2.0);
        const double gB = std::sin(x * std::numbers::pi / 2.0);
        CHECK(std::abs(gA * gA + gB * gB - 1.0) < 1e-12);
    }
}
