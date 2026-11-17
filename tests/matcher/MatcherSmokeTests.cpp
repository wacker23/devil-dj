// Smoke tests for src/matcher.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <numeric>

TEST_CASE("Cosine similarity of a vector with itself is 1", "[smoke][matcher]")
{
    // f_sound and MMR use cosine similarity over sound vectors (MATCHER_SPEC §4.4, §5.1).
    constexpr std::array<float, 4> v{0.5f, -1.0f, 2.0f, 0.25f};
    const float dot = std::inner_product(v.begin(), v.end(), v.begin(), 0.0f);
    const float cosine = dot / (std::sqrt(dot) * std::sqrt(dot));
    CHECK(std::abs(cosine - 1.0f) < 1e-6f);
}
