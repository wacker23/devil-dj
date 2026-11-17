// Smoke tests for src/codebeat.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

TEST_CASE("JS safe integers survive a round trip through double", "[smoke][codebeat]")
{
    // The JS pattern engine's fractions use safe integers (|n| <= 2^53 - 1); the C++ bridge reads them as doubles.
    constexpr std::int64_t kMaxSafeInteger = (std::int64_t{1} << 53) - 1;
    CHECK(static_cast<std::int64_t>(static_cast<double>(kMaxSafeInteger)) == kMaxSafeInteger);
    CHECK(static_cast<std::int64_t>(static_cast<double>(-kMaxSafeInteger)) == -kMaxSafeInteger);
}
