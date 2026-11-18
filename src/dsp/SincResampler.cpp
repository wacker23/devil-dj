#include "dsp/SincResampler.h"

#include "util/ThreadPriority.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <thread>

#if defined(_M_X64) || defined(__SSE2__)
#include <immintrin.h>
#define DEVILDJ_RESAMPLER_SSE 1
#else
#define DEVILDJ_RESAMPLER_SSE 0
#endif

namespace devildj::dsp
{

namespace
{
// Modified Bessel function of the first kind, order 0 (power series).
double besselI0(double x) noexcept
{
    double sum = 1.0;
    double term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 64; ++k)
    {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

// Polyphase kernel table. For an output at input position t = centre + frac (0 <= frac < 1), the taps are
// input samples centre - W + 1 ... centre + W (2W taps). Row p holds the coefficients for frac = p / P,
// contiguous in memory and zero-padded to a multiple of 4 ("stride"), so each output is a pair of SIMD dot
// products (rows p and p+1) blended by the sub-phase.
class PolyphaseKernel
{
public:
    PolyphaseKernel(double scale, int halfWidth, const SincResamplerSettings& s)
        : halfWidth(halfWidth)
        , taps(2 * halfWidth)
        , stride((2 * halfWidth + 3) & ~3)
        , phases(s.phasesPerSample)
        , table(static_cast<std::size_t>(phases + 1) * static_cast<std::size_t>(stride), 0.0f)
    {
        const double betaNorm = besselI0(s.kaiserBeta);
        for (int p = 0; p <= phases; ++p)
        {
            const double frac = static_cast<double>(p) / phases;
            float* row = table.data() + static_cast<std::size_t>(p) * stride;
            for (int k = 0; k < taps; ++k)
            {
                const double x = frac + (halfWidth - 1) - k; // distance from tap to output position
                row[k] = static_cast<float>(h(x, scale, s.kaiserBeta, betaNorm));
            }
        }
    }

    const float* row(int p) const noexcept { return table.data() + static_cast<std::size_t>(p) * stride; }

    int halfWidth;
    int taps;
    int stride; // taps rounded up to a multiple of 4
    int phases;

private:
    double h(double x, double scale, double beta, double betaNorm) const noexcept
    {
        const double r = std::abs(x) / halfWidth;
        if (r >= 1.0)
            return 0.0;
        const double u = scale * x;
        const double sinc = u == 0.0 ? 1.0 : std::sin(std::numbers::pi * u) / (std::numbers::pi * u);
        const double window = besselI0(beta * std::sqrt(1.0 - r * r)) / betaNorm;
        return scale * sinc * window;
    }

    std::vector<float> table;
};

// Dot products of x with rows a and b (n is a multiple of 4).
void dot2(const float* x, const float* a, const float* b, int n, float& outA, float& outB) noexcept
{
#if DEVILDJ_RESAMPLER_SSE
    __m128 accA = _mm_setzero_ps();
    __m128 accB = _mm_setzero_ps();
    for (int i = 0; i < n; i += 4)
    {
        const __m128 v = _mm_loadu_ps(x + i);
        accA = _mm_add_ps(accA, _mm_mul_ps(v, _mm_loadu_ps(a + i)));
        accB = _mm_add_ps(accB, _mm_mul_ps(v, _mm_loadu_ps(b + i)));
    }
    alignas(16) float la[4];
    alignas(16) float lb[4];
    _mm_store_ps(la, accA);
    _mm_store_ps(lb, accB);
    outA = (la[0] + la[1]) + (la[2] + la[3]);
    outB = (lb[0] + lb[1]) + (lb[2] + lb[3]);
#else
    float sa = 0.0f, sb = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        sa += x[i] * a[i];
        sb += x[i] * b[i];
    }
    outA = sa;
    outB = sb;
#endif
}

void resampleRange(const PolyphaseKernel& kernel, const float* paddedStart, double step, std::int64_t from,
                   std::int64_t to, float* output) noexcept
{
    for (std::int64_t n = from; n < to; ++n)
    {
        const double t = static_cast<double>(n) * step; // position in the input
        const auto centre = static_cast<std::int64_t>(t);
        const double phasePos = (t - static_cast<double>(centre)) * kernel.phases;
        const int p = std::min(static_cast<int>(phasePos), kernel.phases - 1);
        const auto blend = static_cast<float>(phasePos - p);

        float a = 0.0f, b = 0.0f;
        dot2(paddedStart + (centre - kernel.halfWidth + 1), kernel.row(p), kernel.row(p + 1), kernel.stride, a, b);
        output[n] = a + blend * (b - a);
    }
}
} // namespace

std::int64_t resampledLength(std::int64_t inputLength, double inRate, double outRate) noexcept
{
    if (inRate <= 0.0 || outRate <= 0.0)
        return 0;
    return static_cast<std::int64_t>(std::llround(static_cast<double>(inputLength) * outRate / inRate));
}

std::vector<float> resample(std::span<const float> input, double inRate, double outRate,
                            const SincResamplerSettings& settings)
{
    if (inRate <= 0.0 || outRate <= 0.0 || input.empty())
        return {};
    if (inRate == outRate)
        return {input.begin(), input.end()};

    const double ratio = outRate / inRate;                       // output samples per input sample
    const double scale = settings.cutoff * std::min(1.0, ratio); // cutoff relative to the input Nyquist
    // Support in input samples: zeroCrossings of a sinc stretched by 1/min(1, ratio).
    const int halfWidth = static_cast<int>(std::ceil(settings.zeroCrossings / std::min(1.0, ratio)));
    const PolyphaseKernel kernel(scale, halfWidth, settings);

    // Zero-padded copy so every output reads a full (stride-wide) window without bounds checks.
    const auto margin = static_cast<std::size_t>(kernel.stride + 4);
    std::vector<float> padded(input.size() + 2 * margin, 0.0f);
    std::copy(input.begin(), input.end(), padded.begin() + static_cast<std::ptrdiff_t>(margin));
    const float* start = padded.data() + margin;

    const auto outLength = resampledLength(static_cast<std::int64_t>(input.size()), inRate, outRate);
    std::vector<float> output(static_cast<std::size_t>(outLength));
    const double step = inRate / outRate; // input samples per output sample

    // Long signals: split the output across a few threads (each output sample is independent).
    constexpr std::int64_t kSamplesPerThread = 1 << 20;
    const auto hardware = static_cast<std::int64_t>(std::max(1u, std::thread::hardware_concurrency()));
    const auto threads =
        std::clamp<std::int64_t>(outLength / kSamplesPerThread, 1, std::min<std::int64_t>(hardware, 8));
    if (threads == 1)
    {
        resampleRange(kernel, start, step, 0, outLength, output.data());
        return output;
    }

    std::vector<std::thread> workers;
    const auto chunk = (outLength + threads - 1) / threads;
    const int priority = util::currentThreadPriority(); // workers run at the caller's priority (loader: low)
    for (std::int64_t i = 0; i < threads; ++i)
    {
        const auto from = i * chunk;
        const auto to = std::min(outLength, from + chunk);
        workers.emplace_back(
            [&, from, to, priority]
            {
                util::setCurrentThreadPriority(priority);
                resampleRange(kernel, start, step, from, to, output.data());
            });
    }
    for (auto& w : workers)
        w.join();
    return output;
}

} // namespace devildj::dsp
