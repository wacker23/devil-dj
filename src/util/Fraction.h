#pragma once

#include <atomic>
#include <cassert>
#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <type_traits>

namespace devildj::util
{

// Exact rational number with 64-bit numerator and denominator (P0-12). Used for beat positions and
// lengths (beat jump 1/8, loop 3/4, pattern cycles) where doubles would drift.
//
// Invariants: denominator > 0, gcd(|numerator|, denominator) == 1, numerator != INT64_MIN.
// Trivially copyable, noexcept, no allocation: safe on the audio thread.
//
// Overflow: every operation is checked. If an exact result doesn't fit in 64 bits, the result is the
// closest fraction with denominator <= 2^30 to the true value, and overflowCount() is incremented.
// Values seen in practice (beats, bars, cycles) are nowhere near that limit.
class Fraction
{
public:
    using Int = std::int64_t;

    constexpr Fraction() noexcept = default;

    // Implicit from integers: Fraction x = 4; means 4/1.
    constexpr Fraction(Int value) noexcept // NOLINT(google-explicit-constructor)
        : num(value == kMinInt ? -kMaxInt : value)
    {
    }

    // denominator must be non-zero (asserted; a zero denominator gives 0).
    constexpr Fraction(Int numerator, Int denominator) noexcept { assign(numerator, denominator); }

    constexpr Int numerator() const noexcept { return num; }
    constexpr Int denominator() const noexcept { return den; }

    constexpr double toDouble() const noexcept { return static_cast<double>(num) / static_cast<double>(den); }

    // Largest integer <= value.
    constexpr Int floor() const noexcept
    {
        const Int q = num / den;
        return (num % den != 0 && num < 0) ? q - 1 : q;
    }

    // Smallest integer >= value.
    constexpr Int ceil() const noexcept
    {
        const Int q = num / den;
        return (num % den != 0 && num > 0) ? q + 1 : q;
    }

    // value - floor(value), in [0, 1).
    constexpr Fraction fractionalPart() const noexcept { return Fraction::fromNormalised(num - floor() * den, den); }

    constexpr bool isInteger() const noexcept { return den == 1; }

    // Closest fraction to 'value' with denominator <= maxDenominator (continued fractions).
    static constexpr Fraction fromDouble(double value, Int maxDenominator = Int{1} << 30) noexcept;

    // Number of operations whose exact result overflowed since start (diagnostics).
    static std::uint64_t overflowCount() noexcept { return overflows.load(std::memory_order_relaxed); }

    std::string toString() const; // "3/4", or "3" for integers

    // ---- arithmetic ----

    constexpr Fraction operator-() const noexcept { return fromNormalised(-num, den); }
    constexpr Fraction operator+() const noexcept { return *this; }

    friend constexpr Fraction operator+(Fraction a, Fraction b) noexcept
    {
        // a/b + c/d = (a·(d/g) + c·(b/g)) / (b/g·d), g = gcd(b, d)
        const Int g = std::gcd(a.den, b.den);
        Int left = 0, right = 0, sum = 0, den = 0;
        if (mulChecked(a.num, b.den / g, left) && mulChecked(b.num, a.den / g, right) && addChecked(left, right, sum) &&
            mulChecked(a.den / g, b.den, den))
            return Fraction(sum, den);
        return overflowFallback(a.toLongDouble() + b.toLongDouble());
    }

    friend constexpr Fraction operator-(Fraction a, Fraction b) noexcept { return a + (-b); }

    friend constexpr Fraction operator*(Fraction a, Fraction b) noexcept
    {
        // Cross-cancel first so intermediate products stay small.
        const Int g1 = std::gcd(a.num, b.den);
        const Int g2 = std::gcd(b.num, a.den);
        const Int n1 = g1 != 0 ? a.num / g1 : a.num;
        const Int d2 = g1 != 0 ? b.den / g1 : b.den;
        const Int n2 = g2 != 0 ? b.num / g2 : b.num;
        const Int d1 = g2 != 0 ? a.den / g2 : a.den;
        Int n = 0, d = 0;
        if (mulChecked(n1, n2, n) && mulChecked(d1, d2, d))
            return fromNormalised(n, d);
        return overflowFallback(a.toLongDouble() * b.toLongDouble());
    }

    // Division by zero is asserted and gives 0.
    friend constexpr Fraction operator/(Fraction a, Fraction b) noexcept
    {
        if (b.num == 0)
        {
            assert(false && "Fraction division by zero");
            return {};
        }
        return a * b.reciprocal();
    }

    constexpr Fraction reciprocal() const noexcept
    {
        assert(num != 0 && "Fraction reciprocal of zero");
        if (num == 0)
            return {};
        return num > 0 ? fromNormalised(den, num) : fromNormalised(-den, -num);
    }

    constexpr Fraction& operator+=(Fraction o) noexcept { return *this = *this + o; }
    constexpr Fraction& operator-=(Fraction o) noexcept { return *this = *this - o; }
    constexpr Fraction& operator*=(Fraction o) noexcept { return *this = *this * o; }
    constexpr Fraction& operator/=(Fraction o) noexcept { return *this = *this / o; }

    // ---- comparison (exact, using 128-bit cross products) ----

    friend constexpr bool operator==(Fraction a, Fraction b) noexcept { return a.num == b.num && a.den == b.den; }

    friend constexpr std::strong_ordering operator<=>(Fraction a, Fraction b) noexcept
    {
        return compareProducts(a.num, b.den, b.num, a.den);
    }

private:
    static constexpr Int kMaxInt = std::numeric_limits<Int>::max();
    static constexpr Int kMinInt = std::numeric_limits<Int>::min();

    inline static std::atomic<std::uint64_t> overflows{0};

    Int num = 0;
    Int den = 1;

    constexpr void assign(Int n, Int d) noexcept
    {
        assert(d != 0 && "Fraction with zero denominator");
        if (d == 0)
        {
            num = 0;
            den = 1;
            return;
        }
        // Normalise the sign without negating INT64_MIN.
        if (n == kMinInt || d == kMinInt)
        {
            // Halve both if possible (keeps the value), otherwise approximate.
            if ((n % 2 == 0) && (d % 2 == 0))
            {
                assign(n / 2, d / 2);
                return;
            }
            *this = overflowFallback(static_cast<long double>(n) / static_cast<long double>(d));
            return;
        }
        if (d < 0)
        {
            n = -n;
            d = -d;
        }
        const Int g = std::gcd(n, d);
        num = n / g;
        den = d / g;
    }

    // Builds n/d through the normalising constructor (sign + gcd), for results of internal arithmetic.
    static constexpr Fraction fromNormalised(Int n, Int d) noexcept { return Fraction(n, d); }

    constexpr long double toLongDouble() const noexcept
    {
        return static_cast<long double>(num) / static_cast<long double>(den);
    }

    static constexpr Fraction overflowFallback(long double value) noexcept
    {
        if (!std::is_constant_evaluated())
            overflows.fetch_add(1, std::memory_order_relaxed);
        return fromDouble(static_cast<double>(value));
    }

    // ---- checked 64-bit helpers (portable, constexpr) ----

    static constexpr bool addChecked(Int a, Int b, Int& out) noexcept
    {
        if ((b > 0 && a > kMaxInt - b) || (b < 0 && a < kMinInt + 1 - b))
            return false; // also rejects INT64_MIN results
        out = a + b;
        return true;
    }

    static constexpr std::uint64_t magnitude(Int v) noexcept
    {
        return v < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
    }

    // Full 64x64 -> 128 unsigned multiply from 32-bit halves.
    static constexpr void mulWide(std::uint64_t a, std::uint64_t b, std::uint64_t& hi, std::uint64_t& lo) noexcept
    {
        const std::uint64_t aLo = a & 0xFFFFFFFFu, aHi = a >> 32;
        const std::uint64_t bLo = b & 0xFFFFFFFFu, bHi = b >> 32;
        const std::uint64_t ll = aLo * bLo;
        const std::uint64_t lh = aLo * bHi;
        const std::uint64_t hl = aHi * bLo;
        const std::uint64_t hh = aHi * bHi;
        const std::uint64_t mid = (ll >> 32) + (lh & 0xFFFFFFFFu) + (hl & 0xFFFFFFFFu);
        lo = (mid << 32) | (ll & 0xFFFFFFFFu);
        hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    }

    // a*b into out; false if it doesn't fit (results equal to INT64_MIN count as not fitting).
    static constexpr bool mulChecked(Int a, Int b, Int& out) noexcept
    {
        std::uint64_t hi = 0, lo = 0;
        mulWide(magnitude(a), magnitude(b), hi, lo);
        if (hi != 0 || lo > static_cast<std::uint64_t>(kMaxInt))
            return false;
        const bool negative = (a < 0) != (b < 0);
        out = negative ? -static_cast<Int>(lo) : static_cast<Int>(lo);
        return true;
    }

    // Compares a*b with c*d exactly.
    static constexpr std::strong_ordering compareProducts(Int a, Int b, Int c, Int d) noexcept
    {
        // Signs of the products (b and d are denominators > 0, but keep this general).
        const int s1 = (a == 0 || b == 0) ? 0 : ((a < 0) != (b < 0) ? -1 : 1);
        const int s2 = (c == 0 || d == 0) ? 0 : ((c < 0) != (d < 0) ? -1 : 1);
        if (s1 != s2)
            return s1 <=> s2;
        if (s1 == 0)
            return std::strong_ordering::equal;

        std::uint64_t h1 = 0, l1 = 0, h2 = 0, l2 = 0;
        mulWide(magnitude(a), magnitude(b), h1, l1);
        mulWide(magnitude(c), magnitude(d), h2, l2);
        const auto byMagnitude = (h1 != h2) ? (h1 <=> h2) : (l1 <=> l2);
        return s1 > 0 ? byMagnitude : (0 <=> byMagnitude); // both negative: larger magnitude is smaller
    }
};

static_assert(std::is_trivially_copyable_v<Fraction>);

// ---- out-of-line (constexpr-capable) ----

constexpr Fraction Fraction::fromDouble(double value, Int maxDenominator) noexcept
{
    if (!(value == value)) // NaN
        return {};
    if (maxDenominator < 1)
        maxDenominator = 1;

    constexpr double kLimit = 9.2e18; // just below INT64_MAX
    if (value >= kLimit)
        return Fraction(kMaxInt);
    if (value <= -kLimit)
        return Fraction(-kMaxInt);

    const bool negative = value < 0;
    double x = negative ? -value : value;

    // Continued fraction convergents p/q, stopping before q exceeds maxDenominator.
    Int p0 = 0, q0 = 1, p1 = 1, q1 = 0;
    double rest = x;
    for (int i = 0; i < 64; ++i)
    {
        const double whole = rest < 9.2e18 ? static_cast<double>(static_cast<Int>(rest)) : 9.2e18;
        const Int a = static_cast<Int>(whole);
        Int p2 = 0, q2 = 0, t = 0;
        if (!mulChecked(a, p1, t) || !addChecked(t, p0, p2) || !mulChecked(a, q1, t) || !addChecked(t, q0, q2) ||
            q2 > maxDenominator)
        {
            // Best semiconvergent within the limit.
            if (q1 != 0)
            {
                const Int k = (maxDenominator - q0) / q1;
                const Int ps = p0 + k * p1, qs = q0 + k * q1;
                const double e1 = x - static_cast<double>(p1) / static_cast<double>(q1);
                const double es = x - static_cast<double>(ps) / static_cast<double>(qs);
                if (k > 0 && (es < 0 ? -es : es) < (e1 < 0 ? -e1 : e1))
                    return Fraction(negative ? -ps : ps, qs);
            }
            break;
        }
        p0 = p1;
        q0 = q1;
        p1 = p2;
        q1 = q2;
        const double frac = rest - whole;
        if (frac <= 1e-18 * (x > 1 ? x : 1))
            break;
        rest = 1.0 / frac;
    }
    if (q1 == 0)
        return Fraction(negative ? -p0 : p0, q0 == 0 ? 1 : q0);
    return Fraction(negative ? -p1 : p1, q1);
}

inline std::string Fraction::toString() const
{
    if (den == 1)
        return std::to_string(num);
    return std::to_string(num) + "/" + std::to_string(den);
}

} // namespace devildj::util
