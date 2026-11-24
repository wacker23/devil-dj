// P6-06: editing phrases - add, move, remove on bars, lengths from the grid.

#include "app/PhraseEdit.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace devildj;
using namespace devildj::app;

namespace
{
constexpr double kRate = 48000.0;
// 120 BPM from 0.5 s: a bar is 2 s, bars at 0.5, 2.5, 4.5 ...
const util::Beatgrid kGrid = util::Beatgrid::constant(0.5 * kRate, 120.0, kRate);
const double kEnd = 64.5 * kRate; // 32 bars

Phrases three()
{
    return withBars({{0.5, 0, 0.3}, {16.5, 0, 0.9}, {48.5, 0, 0.5}}, kGrid, kRate, kEnd);
}
} // namespace

TEST_CASE("Phrase editing: lengths counted on the grid", "[app][phrases]")
{
    const auto p = three();
    REQUIRE(p.size() == 3);
    CHECK(p[0].bars == 8);
    CHECK(p[1].bars == 16);
    CHECK(p[2].bars == 8);
}

TEST_CASE("Phrase editing: the nearest bar and the nearest line", "[app][phrases]")
{
    CHECK(nearestBarSeconds(kGrid, 9.0 * kRate, kRate) == 8.5);  // 0.5 s after a bar
    CHECK(nearestBarSeconds(kGrid, 10.0 * kRate, kRate) == 10.5); // 1.5 s after: the next bar
    const auto p = three();
    CHECK(nearestPhrase(p, 17.0, 4.0) == 1);
    CHECK(nearestPhrase(p, 30.0, 4.0) == -1);
}

TEST_CASE("Phrase editing: add splits a phrase, move and remove keep the rest", "[app][phrases]")
{
    auto p = withBars(addPhrase(three(), 32.5), kGrid, kRate, kEnd);
    REQUIRE(p.size() == 4);
    CHECK(p[2].startSeconds == 32.5);
    CHECK(p[1].bars == 8);
    CHECK(p[2].bars == 8);
    CHECK(p[2].energy == 0.9); // the phrase it split
    CHECK(addPhrase(p, 32.5).size() == 4); // one already starts there

    p = withBars(movePhrase(p, 2, 40.5), kGrid, kRate, kEnd);
    CHECK(p[2].startSeconds == 40.5);
    CHECK(p[1].bars == 12);
    CHECK(p[2].bars == 4);
    CHECK(movePhrase(p, 2, 16.5) == p); // onto another start: nothing

    p = withBars(removePhrase(p, 2), kGrid, kRate, kEnd);
    REQUIRE(p.size() == 3);
    CHECK(p[1].bars == 16);
    CHECK(std::abs(p[1].energy - 0.9) < 1e-12); // both parts had 0.9

    p = removePhrase(three(), 1); // 16 bars at 0.9 into 8 at 0.3
    CHECK(std::abs(p[0].energy - (0.3 * 8 + 0.9 * 16) / 24.0) < 1e-12);
    p = removePhrase(three(), 0); // the first: just gone
    CHECK(p.front().startSeconds == 16.5);
}
