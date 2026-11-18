#include "WaveformBuilder.h"

#include "dsp/Biquad.h"

#include <algorithm>
#include <cmath>

namespace devildj::analysis
{

namespace
{
constexpr double kButterworthQ = 0.70710678118654752;

// LR4 = two identical Butterworth 2nd-order sections.
struct Lr4
{
    dsp::BiquadCoeffs k;
    dsp::BiquadState a, b;
    double process(double x) noexcept { return b.process(k, a.process(k, x)); }
};

void overview(const std::vector<float>& peak, const std::vector<float>& rms, std::vector<float>& outPeak,
              std::vector<float>& outRms, int points)
{
    const auto n = static_cast<long long>(peak.size());
    outPeak.assign(static_cast<std::size_t>(points), 0.0f);
    outRms.assign(static_cast<std::size_t>(points), 0.0f);
    for (int o = 0; o < points; ++o)
    {
        const auto from = n * o / points, to = n * (o + 1) / points;
        float p = 0.0f;
        double sumSquares = 0.0;
        for (auto i = from; i < to; ++i)
        {
            p = std::max(p, peak[static_cast<std::size_t>(i)]);
            sumSquares += static_cast<double>(rms[static_cast<std::size_t>(i)]) * rms[static_cast<std::size_t>(i)];
        }
        outPeak[static_cast<std::size_t>(o)] = p;
        outRms[static_cast<std::size_t>(o)] =
            to > from ? static_cast<float>(std::sqrt(sumSquares / (to - from))) : 0.0f;
    }
}
} // namespace

Waveform buildWaveform(const float* left, const float* right, int numSamples, double sampleRate)
{
    Waveform w;
    w.sampleRate = sampleRate;
    w.samplesPerPoint = std::max(1, static_cast<int>(std::lround(sampleRate / Waveform::kPointsPerSecond)));
    if (numSamples <= 0 || left == nullptr || right == nullptr || sampleRate <= 0.0)
    {
        for (int b = 0; b < Waveform::kBands; ++b)
            overview(w.peak[static_cast<std::size_t>(b)], w.rms[static_cast<std::size_t>(b)],
                     w.overviewPeak[static_cast<std::size_t>(b)], w.overviewRms[static_cast<std::size_t>(b)],
                     Waveform::kOverviewPoints);
        w.overviewTotalPeak.assign(Waveform::kOverviewPoints, 0.0f);
        w.overviewTotalRms.assign(Waveform::kOverviewPoints, 0.0f);
        return w;
    }

    const int points = (numSamples + w.samplesPerPoint - 1) / w.samplesPerPoint;
    for (int b = 0; b < Waveform::kBands; ++b)
    {
        w.peak[static_cast<std::size_t>(b)].assign(static_cast<std::size_t>(points), 0.0f);
        w.rms[static_cast<std::size_t>(b)].assign(static_cast<std::size_t>(points), 0.0f);
    }
    std::vector<float> totalPeak(static_cast<std::size_t>(points), 0.0f), totalRms(totalPeak.size(), 0.0f);

    Lr4 lowPass{dsp::BiquadCoeffs::lowPass(250.0, sampleRate, kButterworthQ), {}, {}};
    Lr4 restHigh{dsp::BiquadCoeffs::highPass(250.0, sampleRate, kButterworthQ), {}, {}};
    Lr4 midPass{dsp::BiquadCoeffs::lowPass(2500.0, sampleRate, kButterworthQ), {}, {}};
    Lr4 highPass{dsp::BiquadCoeffs::highPass(2500.0, sampleRate, kButterworthQ), {}, {}};

    for (int p = 0; p < points; ++p)
    {
        const int start = p * w.samplesPerPoint;
        const int end = std::min(numSamples, start + w.samplesPerPoint);
        std::array<double, 3> peak{}, sum{};
        double tPeak = 0.0, tSum = 0.0;
        for (int i = start; i < end; ++i)
        {
            const double x = 0.5 * (static_cast<double>(left[i]) + right[i]);
            const double rest = restHigh.process(x);
            const std::array<double, 3> band{lowPass.process(x), midPass.process(rest), highPass.process(rest)};
            for (std::size_t b = 0; b < 3; ++b)
            {
                peak[b] = std::max(peak[b], std::abs(band[b]));
                sum[b] += band[b] * band[b];
            }
            tPeak = std::max(tPeak, std::abs(x));
            tSum += x * x;
        }
        const double count = std::max(1, end - start);
        for (std::size_t b = 0; b < 3; ++b)
        {
            w.peak[b][static_cast<std::size_t>(p)] = static_cast<float>(peak[b]);
            w.rms[b][static_cast<std::size_t>(p)] = static_cast<float>(std::sqrt(sum[b] / count));
        }
        totalPeak[static_cast<std::size_t>(p)] = static_cast<float>(tPeak);
        totalRms[static_cast<std::size_t>(p)] = static_cast<float>(std::sqrt(tSum / count));
    }

    for (std::size_t b = 0; b < 3; ++b)
        overview(w.peak[b], w.rms[b], w.overviewPeak[b], w.overviewRms[b], Waveform::kOverviewPoints);
    overview(totalPeak, totalRms, w.overviewTotalPeak, w.overviewTotalRms, Waveform::kOverviewPoints);
    return w;
}

} // namespace devildj::analysis
