#include "Loudness.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace devildj::analysis
{

namespace
{
// The analog design behind BS.1770's 48 kHz coefficients (stage 1: shelf frequency, gain, Q; stage 2: high-pass
// frequency, Q).
constexpr double kShelfHz = 1681.974450955533;
constexpr double kShelfGainDb = 3.999843853973347;
constexpr double kShelfQ = 0.7071752369554196;
constexpr double kHighPassHz = 38.13547087602444;
constexpr double kHighPassQ = 0.5003270373238773;

constexpr double kBlockSeconds = 0.4;
constexpr int kStepsPerBlock = 4; // 75 % overlap: a block every 100 ms
constexpr double kAbsoluteGate = -70.0;
constexpr double kRelativeGate = -10.0;

double loudness(double meanSquareSum)
{
    return -0.691 + 10.0 * std::log10(meanSquareSum);
}

// Sum of squares of the K-weighted signal per 100 ms step.
std::vector<double> stepEnergies(const float* x, int n, double sampleRate, int stepLength)
{
    const auto k = kWeighting(sampleRate);
    std::vector<double> steps(static_cast<std::size_t>(n / stepLength), 0.0);
    double s1[2] = {}, s2[2] = {}; // transposed direct form II states
    for (int i = 0; i < static_cast<int>(steps.size()) * stepLength; ++i)
    {
        double v = x[i];
        for (int stage = 0; stage < 2; ++stage)
        {
            const auto& f = k[static_cast<std::size_t>(stage)];
            const double y = f.b0 * v + s1[stage];
            s1[stage] = f.b1 * v - f.a1 * y + s2[stage];
            s2[stage] = f.b2 * v - f.a2 * y;
            v = y;
        }
        steps[static_cast<std::size_t>(i / stepLength)] += v * v;
    }
    return steps;
}
} // namespace

std::array<Biquad, 2> kWeighting(double sampleRate)
{
    std::array<Biquad, 2> out;
    {
        const double k = std::tan(std::numbers::pi * kShelfHz / sampleRate);
        const double vh = std::pow(10.0, kShelfGainDb / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / kShelfQ + k * k;
        out[0].b0 = (vh + vb * k / kShelfQ + k * k) / a0;
        out[0].b1 = 2.0 * (k * k - vh) / a0;
        out[0].b2 = (vh - vb * k / kShelfQ + k * k) / a0;
        out[0].a1 = 2.0 * (k * k - 1.0) / a0;
        out[0].a2 = (1.0 - k / kShelfQ + k * k) / a0;
    }
    {
        const double k = std::tan(std::numbers::pi * kHighPassHz / sampleRate);
        const double a0 = 1.0 + k / kHighPassQ + k * k;
        out[1].b0 = 1.0;
        out[1].b1 = -2.0;
        out[1].b2 = 1.0;
        out[1].a1 = 2.0 * (k * k - 1.0) / a0;
        out[1].a2 = (1.0 - k / kHighPassQ + k * k) / a0;
    }
    return out;
}

std::optional<double> integratedLoudness(const float* left, const float* right, int numSamples, double sampleRate)
{
    if (left == nullptr || sampleRate <= 0.0)
        return std::nullopt;
    const int stepLength = static_cast<int>(std::lround(kBlockSeconds * sampleRate / kStepsPerBlock));
    if (stepLength <= 0 || numSamples < stepLength * kStepsPerBlock)
        return std::nullopt;
    auto steps = stepEnergies(left, numSamples, sampleRate, stepLength);
    if (right != nullptr)
    {
        const auto r = stepEnergies(right, numSamples, sampleRate, stepLength);
        for (std::size_t i = 0; i < steps.size(); ++i)
            steps[i] += r[i]; // channel weights 1 (left, right)
    }

    // Blocks: the mean square of four consecutive steps (each channel's, summed).
    const double blockLength = static_cast<double>(stepLength) * kStepsPerBlock;
    std::vector<double> blocks;
    for (std::size_t i = 0; i + kStepsPerBlock <= steps.size(); ++i)
        blocks.push_back((steps[i] + steps[i + 1] + steps[i + 2] + steps[i + 3]) / blockLength);

    const auto gatedMean = [&](double threshold) -> std::optional<double>
    {
        double sum = 0.0;
        int count = 0;
        for (const double z : blocks)
            if (z > 0.0 && loudness(z) > threshold)
            {
                sum += z;
                ++count;
            }
        if (count == 0)
            return std::nullopt;
        return sum / count;
    };
    const auto absolute = gatedMean(kAbsoluteGate);
    if (!absolute)
        return std::nullopt;
    const double relativeThreshold = loudness(*absolute) + kRelativeGate;
    const auto gated = gatedMean(std::max(kAbsoluteGate, relativeThreshold));
    if (!gated)
        return std::nullopt;
    return loudness(*gated);
}

double autoGainDb(double trackLufs, double targetLufs, double maxDb)
{
    return std::clamp(targetLufs - trackLufs, -maxDb, maxDb);
}

} // namespace devildj::analysis
