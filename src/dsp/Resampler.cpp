#include "dsp/Resampler.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace devildj::dsp
{

namespace
{
constexpr double kKaiserBeta = 7.0;
constexpr double kCutoff = 0.9; // of the output Nyquist at rate 1

double besselI0(double x) noexcept
{
    double sum = 1.0, term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 50; ++k)
    {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < sum * 1e-16)
            break;
    }
    return sum;
}

inline float sampleAt(const float* data, int length, std::int64_t i) noexcept
{
    return (i >= 0 && i < length) ? data[i] : 0.0f;
}
} // namespace

void Resampler::prepare()
{
    constexpr int half = kTaps / 2;
    const double betaNorm = besselI0(kKaiserBeta);
    sincTables.assign(kRateBuckets.size() * (kPhases + 1) * kTaps, 0.0f);

    for (std::size_t b = 0; b < kRateBuckets.size(); ++b)
    {
        const double scale = kCutoff / kRateBuckets[b]; // narrower low-pass when reading faster
        for (int p = 0; p <= kPhases; ++p)
        {
            const double frac = static_cast<double>(p) / kPhases;
            float* row = sincTables.data() + (b * (kPhases + 1) + static_cast<std::size_t>(p)) * kTaps;
            double sum = 0.0;
            for (int k = 0; k < kTaps; ++k)
            {
                const double x = frac + (half - 1) - k; // distance from tap to the read position
                const double r = x / half;
                const double window =
                    std::abs(r) < 1.0 ? besselI0(kKaiserBeta * std::sqrt(1.0 - r * r)) / betaNorm : 0.0;
                const double u = scale * x;
                const double sinc = u == 0.0 ? 1.0 : std::sin(std::numbers::pi * u) / (std::numbers::pi * u);
                row[k] = static_cast<float>(scale * sinc * window);
                sum += row[k];
            }
            for (int k = 0; k < kTaps; ++k) // unity DC gain at every phase
                row[k] = static_cast<float>(row[k] / sum);
        }
    }
}

float Resampler::readHermite(const float* data, int length, double position) noexcept
{
    const auto i = static_cast<std::int64_t>(std::floor(position));
    const auto t = static_cast<float>(position - static_cast<double>(i));
    const float xm1 = sampleAt(data, length, i - 1);
    const float x0 = sampleAt(data, length, i);
    const float x1 = sampleAt(data, length, i + 1);
    const float x2 = sampleAt(data, length, i + 2);

    // Catmull-Rom style 4-point, 3rd-order Hermite.
    const float c0 = x0;
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + c0;
}

float Resampler::readSinc(const float* data, int length, double position, double rate) const noexcept
{
    if (sincTables.empty())
        return readHermite(data, length, position); // not prepared

    std::size_t bucket = kRateBuckets.size() - 1;
    const double absRate = std::abs(rate);
    for (std::size_t b = 0; b < kRateBuckets.size(); ++b)
        if (absRate <= kRateBuckets[b] + 1e-9)
        {
            bucket = b;
            break;
        }

    const auto centre = static_cast<std::int64_t>(std::floor(position));
    const double phasePos = (position - static_cast<double>(centre)) * kPhases;
    const int p = std::min(static_cast<int>(phasePos), kPhases - 1);
    const auto blend = static_cast<float>(phasePos - p);
    const float* rowA = sincTables.data() + (bucket * (kPhases + 1) + static_cast<std::size_t>(p)) * kTaps;
    const float* rowB = rowA + kTaps;

    constexpr int half = kTaps / 2;
    const std::int64_t first = centre - half + 1;
    float a = 0.0f, b = 0.0f;
    if (first >= 0 && first + kTaps <= length)
    {
        const float* x = data + first;
        for (int k = 0; k < kTaps; ++k)
        {
            a += x[k] * rowA[k];
            b += x[k] * rowB[k];
        }
    }
    else
    {
        for (int k = 0; k < kTaps; ++k)
        {
            const float v = sampleAt(data, length, first + k);
            a += v * rowA[k];
            b += v * rowB[k];
        }
    }
    return a + blend * (b - a);
}

} // namespace devildj::dsp
