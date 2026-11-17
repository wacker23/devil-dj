#include "util/Fraction.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>

using devildj::util::Fraction;
using Int = Fraction::Int;

namespace
{
bool isNormalised(Fraction f)
{
    return f.denominator() > 0 && std::gcd(f.numerator(), f.denominator()) == 1 &&
           f.numerator() != std::numeric_limits<Int>::min();
}

// Random fractions small enough that every identity below stays exact (no overflow fallback).
struct RandomFractions
{
    explicit RandomFractions(std::uint64_t seed)
        : rng(seed)
    {
    }

    Fraction next() { return Fraction(numerator(rng), denominator(rng)); }

    Fraction nextNonZero()
    {
        for (;;)
            if (auto f = next(); f != Fraction(0))
                return f;
    }

    std::mt19937_64 rng;
    std::uniform_int_distribution<Int> numerator{-1'000'000, 1'000'000};
    std::uniform_int_distribution<Int> denominator{1, 100'000};
};

constexpr int kIterations = 100'000;
} // namespace

// ---- basics ----

TEST_CASE("Fraction normalises sign and common factors", "[util][Fraction]")
{
    CHECK(Fraction(6, 8) == Fraction(3, 4));
    CHECK(Fraction(3, -4).numerator() == -3);
    CHECK(Fraction(3, -4).denominator() == 4);
    CHECK(Fraction(-3, -4) == Fraction(3, 4));
    CHECK(Fraction(0, -5) == Fraction(0));
    CHECK(Fraction(0, -5).denominator() == 1);
    CHECK(Fraction() == Fraction(0, 1));
    CHECK(Fraction(7) == Fraction(14, 2));
}

TEST_CASE("Fraction arithmetic on known values", "[util][Fraction]")
{
    CHECK(Fraction(1, 2) + Fraction(1, 3) == Fraction(5, 6));
    CHECK(Fraction(1, 2) - Fraction(3, 4) == Fraction(-1, 4));
    CHECK(Fraction(2, 3) * Fraction(9, 4) == Fraction(3, 2));
    CHECK(Fraction(1, 8) / Fraction(1, 4) == Fraction(1, 2));
    CHECK(-Fraction(1, 8) == Fraction(-1, 8));
    CHECK(Fraction(3, 4).reciprocal() == Fraction(4, 3));
    CHECK(Fraction(-3, 4).reciprocal() == Fraction(-4, 3));

    Fraction beat = 0;
    for (int i = 0; i < 8; ++i)
        beat += Fraction(1, 8); // eight 1/8-beat jumps land exactly on one beat
    CHECK(beat == Fraction(1));
}

TEST_CASE("Fraction floor, ceil and fractional part", "[util][Fraction]")
{
    CHECK(Fraction(7, 2).floor() == 3);
    CHECK(Fraction(7, 2).ceil() == 4);
    CHECK(Fraction(-7, 2).floor() == -4);
    CHECK(Fraction(-7, 2).ceil() == -3);
    CHECK(Fraction(6, 2).floor() == 3);
    CHECK(Fraction(6, 2).ceil() == 3);
    CHECK(Fraction(-7, 2).fractionalPart() == Fraction(1, 2));
    CHECK(Fraction(9, 4).fractionalPart() == Fraction(1, 4));
    CHECK(Fraction(3).isInteger());
    CHECK_FALSE(Fraction(3, 2).isInteger());
}

TEST_CASE("Fraction comparison", "[util][Fraction]")
{
    CHECK(Fraction(1, 3) < Fraction(1, 2));
    CHECK(Fraction(-1, 2) < Fraction(-1, 3));
    CHECK(Fraction(2, 4) == Fraction(1, 2));
    CHECK(Fraction(5, 3) > 1);
    CHECK(Fraction(-5, 3) < 0);
    CHECK(Fraction(0) <= Fraction(0));
}

TEST_CASE("Fraction toDouble and toString", "[util][Fraction]")
{
    CHECK(Fraction(3, 4).toDouble() == 0.75);
    CHECK(Fraction(-1, 8).toDouble() == -0.125);
    CHECK(Fraction(3, 4).toString() == "3/4");
    CHECK(Fraction(-6, 3).toString() == "-2");
}

TEST_CASE("Fraction::fromDouble finds simple fractions", "[util][Fraction]")
{
    CHECK(Fraction::fromDouble(0.75) == Fraction(3, 4));
    CHECK(Fraction::fromDouble(-0.125) == Fraction(-1, 8));
    CHECK(Fraction::fromDouble(1.0 / 3.0) == Fraction(1, 3));
    CHECK(Fraction::fromDouble(128.0) == Fraction(128));
    CHECK(Fraction::fromDouble(0.0) == Fraction(0));
    CHECK(Fraction::fromDouble(3.14159265358979, 1000) == Fraction(355, 113));
    CHECK(Fraction::fromDouble(std::nan("")) == Fraction(0));
}

TEST_CASE("Fraction is usable at compile time", "[util][Fraction]")
{
    constexpr Fraction a(1, 4);
    constexpr Fraction b = a + Fraction(1, 4);
    STATIC_CHECK(b == Fraction(1, 2));
    STATIC_CHECK((a * 4).isInteger());
    STATIC_CHECK(Fraction(7, 2).floor() == 3);
    STATIC_CHECK(std::is_trivially_copyable_v<Fraction>);
}

// ---- property tests (random values, fixed seeds so failures reproduce) ----

TEST_CASE("Fraction property: results are always normalised", "[util][Fraction][property]")
{
    RandomFractions gen(1);
    for (int i = 0; i < kIterations; ++i)
    {
        const auto a = gen.next();
        const auto b = gen.nextNonZero();
        if (!isNormalised(a + b) || !isNormalised(a - b) || !isNormalised(a * b) || !isNormalised(a / b))
            FAIL("not normalised for a=" << a.toString() << " b=" << b.toString());
    }
}

TEST_CASE("Fraction property: field identities", "[util][Fraction][property]")
{
    RandomFractions gen(2);
    const auto overflowsBefore = Fraction::overflowCount();
    int failures = 0;
    for (int i = 0; i < kIterations; ++i)
    {
        const auto a = gen.next();
        const auto b = gen.next();
        const auto c = gen.next();
        const auto nz = gen.nextNonZero();

        const bool ok = (a + b == b + a)                                // commutative +
                        && (a * b == b * a)                             // commutative ×
                        && ((a + b) + c == a + (b + c))                 // associative +
                        && ((a * b) * c == a * (b * c))                 // associative ×
                        && (a * (b + c) == a * b + a * c)               // distributive
                        && (a + 0 == a) && (a * 1 == a) && (a * 0 == 0) // identities
                        && (a - a == 0) && (-(-a) == a)                 // inverses
                        && (nz / nz == 1) && (nz * nz.reciprocal() == 1) && ((a / nz) * nz == a) && (a - b == -(b - a));
        if (!ok && ++failures <= 5)
            UNSCOPED_INFO("identity failed for a=" << a.toString() << " b=" << b.toString() << " c=" << c.toString()
                                                   << " nz=" << nz.toString());
    }
    CHECK(failures == 0);
    CHECK(Fraction::overflowCount() == overflowsBefore); // inputs chosen so nothing overflowed
}

TEST_CASE("Fraction property: ordering matches the real line", "[util][Fraction][property]")
{
    RandomFractions gen(3);
    int failures = 0;
    for (int i = 0; i < kIterations; ++i)
    {
        const auto a = gen.next();
        const auto b = gen.next();
        const auto c = gen.next();

        const int trichotomy = (a < b ? 1 : 0) + (a == b ? 1 : 0) + (a > b ? 1 : 0);
        const bool ok = trichotomy == 1 && ((a < b) == (b - a > 0)) // order agrees with subtraction
                        && (!(a < b && b < c) || a < c)             // transitive
                        && ((a < b) == (a + c < b + c))             // translation invariant
                        && (std::abs(a.toDouble() - b.toDouble()) < 1e-9 || (a < b) == (a.toDouble() < b.toDouble()));
        if (!ok)
            ++failures;
    }
    CHECK(failures == 0);
}

TEST_CASE("Fraction property: floor and fractional part", "[util][Fraction][property]")
{
    RandomFractions gen(4);
    int failures = 0;
    for (int i = 0; i < kIterations; ++i)
    {
        const auto a = gen.next();
        const Fraction fl = a.floor();
        const bool ok = fl <= a && a < fl + 1 && Fraction(a.ceil()) >= a && Fraction(a.ceil()) - fl <= 1 &&
                        a.fractionalPart() >= 0 && a.fractionalPart() < 1 && fl + a.fractionalPart() == a &&
                        std::abs(a.toDouble() - static_cast<double>(a.numerator()) / a.denominator()) == 0.0;
        if (!ok)
            ++failures;
    }
    CHECK(failures == 0);
}

TEST_CASE("Fraction property: fromDouble round-trips small fractions", "[util][Fraction][property]")
{
    std::mt19937_64 rng(5);
    std::uniform_int_distribution<Int> num(-10'000, 10'000);
    std::uniform_int_distribution<Int> den(1, 1'000);
    int failures = 0;
    for (int i = 0; i < kIterations; ++i)
    {
        const Fraction a(num(rng), den(rng));
        if (Fraction::fromDouble(a.toDouble(), 1'000) != a)
            ++failures;
    }
    CHECK(failures == 0);
}

// ---- overflow ----

TEST_CASE("Fraction overflow falls back to the nearest representable value", "[util][Fraction]")
{
    const auto before = Fraction::overflowCount();
    const Fraction big(std::numeric_limits<Int>::max() / 2, 3);
    const Fraction product = big * big; // ~1e37: cannot be exact

    CHECK(Fraction::overflowCount() > before);
    CHECK(isNormalised(product));
    CHECK(product == Fraction(std::numeric_limits<Int>::max())); // saturates at the largest value

    const Fraction tiny(1, std::numeric_limits<Int>::max() - 1);
    const Fraction sum = tiny + Fraction(1, std::numeric_limits<Int>::max() - 2); // denominators don't fit
    CHECK(isNormalised(sum));
    CHECK(std::abs(sum.toDouble() - 2.0 / 9.2e18) < 1e-18);
}

TEST_CASE("Fraction comparison stays exact beyond 64-bit products", "[util][Fraction]")
{
    const Int m = std::numeric_limits<Int>::max();
    // x/(x+1) grows with x; the cross products here are ~8.5e37, far beyond int64.
    CHECK(Fraction(m - 2, m - 1) < Fraction(m - 1, m));
    CHECK_FALSE(Fraction(m - 1, m) < Fraction(m - 2, m - 1));
    CHECK(Fraction(-(m - 1), m) > Fraction(-(m - 2), m - 3)); // -0.99.. > -1.00..
}

TEST_CASE("Fraction handles INT64_MIN input safely", "[util][Fraction]")
{
    const Int minValue = std::numeric_limits<Int>::min();
    CHECK(isNormalised(Fraction(minValue)));
    CHECK(isNormalised(Fraction(minValue, 2)));
    CHECK(Fraction(minValue, 2) == Fraction(minValue / 2));
    CHECK(isNormalised(Fraction(1, minValue))); // not exactly representable: nearest value (~ -1e-19 -> 0)
    CHECK(Fraction(1, minValue).toDouble() <= 0.0);
}
