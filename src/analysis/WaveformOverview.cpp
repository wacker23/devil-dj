#include "WaveformOverview.h"

#include <algorithm>
#include <cmath>

namespace devildj::analysis
{

WaveformOverview buildOverview(const float* left, const float* right, int numSamples, int points)
{
    WaveformOverview o;
    if (points <= 0)
        return o;
    o.peak.assign(static_cast<std::size_t>(points), 0.0f);
    o.rms.assign(static_cast<std::size_t>(points), 0.0f);
    if (numSamples <= 0 || left == nullptr || right == nullptr)
        return o;

    for (int p = 0; p < points; ++p)
    {
        // Bucket p covers [start, end): every sample lands in exactly one bucket.
        const auto start = static_cast<int>(static_cast<long long>(numSamples) * p / points);
        const auto end = static_cast<int>(static_cast<long long>(numSamples) * (p + 1) / points);
        float peak = 0.0f;
        double sum = 0.0;
        for (int i = start; i < end; ++i)
        {
            const float l = left[i], r = right[i];
            peak = std::max({peak, std::abs(l), std::abs(r)});
            sum += 0.5 * (static_cast<double>(l) * l + static_cast<double>(r) * r);
        }
        o.peak[static_cast<std::size_t>(p)] = peak;
        o.rms[static_cast<std::size_t>(p)] = end > start ? static_cast<float>(std::sqrt(sum / (end - start))) : 0.0f;
    }
    return o;
}

} // namespace devildj::analysis
