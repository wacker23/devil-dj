// Smoke tests for src/beatmaker.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>

TEST_CASE("Seeded RNG is reproducible", "[smoke][beatmaker]")
{
    // Step probability uses a seeded RNG so a pattern plays the same way every time (BEATMAKER_SPEC).
    std::mt19937 a(1234u);
    std::mt19937 b(1234u);
    for (int i = 0; i < 100; ++i)
        CHECK(a() == b());
}
