#include "analysis/PhraseDetector.h"

#include "analysis/Fft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <numeric>

namespace devildj::analysis
{

namespace
{
constexpr int kFftSize = 2048;
constexpr int kHop = 1024;
constexpr int kFeatures = 12 + 3; // chroma + three band levels

struct BarFeatures
{
    std::array<double, 12> chroma{};
    std::array<double, 3> bandDb{};
    double levelDb = -100.0;
    double flux = 0.0; // onset density: mean positive spectral flux per frame
};

double toDb(double power) noexcept
{
    return 10.0 * std::log10(power + 1e-12);
}

// Frames' spectra summed per bar.
std::vector<BarFeatures> barFeatures(const float* mono, int n, double rate, const std::vector<double>& bars)
{
    std::vector<BarFeatures> out(bars.size() > 0 ? bars.size() - 1 : 0);
    if (out.empty())
        return out;
    const Fft fft(kFftSize);
    std::vector<float> window(kFftSize), frame(kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        window[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / (kFftSize - 1)));
    const double binHz = rate / kFftSize;
    // Pitch class of every bin in the chroma range (-1 outside).
    std::vector<int> pitchClass(kFftSize / 2 + 1, -1);
    for (int k = 1; k <= kFftSize / 2; ++k)
    {
        const double hz = k * binHz;
        if (hz < 80.0 || hz > 5000.0)
            continue;
        const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
        pitchClass[static_cast<std::size_t>(k)] = ((static_cast<int>(std::lround(midi)) % 12) + 12) % 12;
    }
    const int lowEnd = static_cast<int>(150.0 / binHz), midEnd = static_cast<int>(2000.0 / binHz);

    std::vector<double> mag, previous(kFftSize / 2 + 1, 0.0);
    std::vector<std::complex<double>> work;
    std::vector<int> frames(out.size(), 0);
    std::vector<std::array<double, 3>> bandPower(out.size());
    std::vector<double> power(out.size(), 0.0);
    std::size_t bar = 0;
    for (int start = 0; start + kFftSize <= n; start += kHop)
    {
        const double centre = (start + kFftSize / 2) / rate;
        while (bar < out.size() && centre >= bars[bar + 1])
            ++bar;
        if (bar >= out.size())
            break;
        if (centre < bars[0])
            continue;
        double energy = 0.0;
        for (int i = 0; i < kFftSize; ++i)
        {
            const float v = mono[start + i];
            energy += static_cast<double>(v) * v;
            frame[static_cast<std::size_t>(i)] = v * window[static_cast<std::size_t>(i)];
        }
        fft.magnitudes(frame.data(), mag, work);
        auto& f = out[bar];
        double flux = 0.0;
        for (int k = 1; k <= kFftSize / 2; ++k)
        {
            const double m = mag[static_cast<std::size_t>(k)];
            const double p = m * m;
            if (const int pc = pitchClass[static_cast<std::size_t>(k)]; pc >= 0)
                f.chroma[static_cast<std::size_t>(pc)] += m;
            bandPower[bar][k < lowEnd ? 0 : k < midEnd ? 1 : 2] += p;
            const double lm = std::log1p(100.0 * m), lp = std::log1p(100.0 * previous[static_cast<std::size_t>(k)]);
            flux += std::max(0.0, lm - lp);
            previous[static_cast<std::size_t>(k)] = m;
        }
        f.flux += flux;
        power[bar] += energy / kFftSize;
        ++frames[bar];
    }
    for (std::size_t b = 0; b < out.size(); ++b)
    {
        auto& f = out[b];
        const int count = std::max(1, frames[b]);
        double norm = 0.0;
        for (const double c : f.chroma)
            norm += c * c;
        norm = std::sqrt(norm);
        for (auto& c : f.chroma)
            c = norm > 0.0 ? c / norm : 0.0;
        for (int k = 0; k < 3; ++k)
            f.bandDb[static_cast<std::size_t>(k)] = toDb(bandPower[b][static_cast<std::size_t>(k)] / count);
        f.levelDb = toDb(power[b] / count);
        f.flux /= count;
    }
    return out;
}

// Feature vectors for the self-similarity: chroma as it is, band levels z-scored over the track (scaled to weigh
// about as much as the chroma).
std::vector<std::array<double, kFeatures>> vectors(const std::vector<BarFeatures>& bars)
{
    std::array<double, 3> mean{}, sd{};
    for (const auto& b : bars)
        for (int k = 0; k < 3; ++k)
            mean[static_cast<std::size_t>(k)] += b.bandDb[static_cast<std::size_t>(k)];
    for (auto& m : mean)
        m /= static_cast<double>(bars.size());
    for (const auto& b : bars)
        for (int k = 0; k < 3; ++k)
        {
            const double d = b.bandDb[static_cast<std::size_t>(k)] - mean[static_cast<std::size_t>(k)];
            sd[static_cast<std::size_t>(k)] += d * d;
        }
    for (auto& s : sd)
        s = std::max(3.0, std::sqrt(s / static_cast<double>(bars.size()))); // 3 dB floor: steady tracks stay calm
    std::vector<std::array<double, kFeatures>> v(bars.size());
    for (std::size_t i = 0; i < bars.size(); ++i)
    {
        for (int k = 0; k < 12; ++k)
            v[i][static_cast<std::size_t>(k)] = bars[i].chroma[static_cast<std::size_t>(k)];
        for (int k = 0; k < 3; ++k)
            v[i][static_cast<std::size_t>(12 + k)] =
                0.35 * (bars[i].bandDb[static_cast<std::size_t>(k)] - mean[static_cast<std::size_t>(k)]) / sd[static_cast<std::size_t>(k)];
    }
    return v;
}

double similarity(const std::array<double, kFeatures>& a, const std::array<double, kFeatures>& b) noexcept
{
    double d = 0.0;
    for (int k = 0; k < kFeatures; ++k)
    {
        const double x = a[static_cast<std::size_t>(k)] - b[static_cast<std::size_t>(k)];
        d += x * x;
    }
    return std::exp(-d / 0.5);
}

// Foote novelty per bar boundary (a boundary at the start of bar i), 0..1.
std::vector<double> novelty(const std::vector<std::array<double, kFeatures>>& v, int kernelBars)
{
    const int n = static_cast<int>(v.size());
    const int half = std::max(1, kernelBars / 2);
    std::vector<double> out(static_cast<std::size_t>(n), 0.0);
    std::vector<double> sim(static_cast<std::size_t>(n) * static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        for (int j = i; j < n; ++j)
            sim[static_cast<std::size_t>(i) * n + j] = sim[static_cast<std::size_t>(j) * n + i] = similarity(v[static_cast<std::size_t>(i)], v[static_cast<std::size_t>(j)]);
    const auto s = [&](int i, int j) { return sim[static_cast<std::size_t>(i) * n + j]; };
    const double sigma = 0.5 * half;
    for (int c = 1; c < n; ++c)
    {
        double sum = 0.0, weight = 0.0;
        for (int a = -half; a < half; ++a)
            for (int b = -half; b < half; ++b)
            {
                const int i = c + a, j = c + b;
                if (i < 0 || j < 0 || i >= n || j >= n)
                    continue;
                const double ai = a + 0.5, bj = b + 0.5;
                const double g = std::exp(-(ai * ai + bj * bj) / (2.0 * sigma * sigma));
                const double sign = (a < 0) == (b < 0) ? 1.0 : -1.0; // same side: alike is good; across: alike is bad
                sum += sign * g * s(i, j);
                weight += g;
            }
        out[static_cast<std::size_t>(c)] = weight > 0.0 ? std::max(0.0, sum / weight) : 0.0;
    }
    const double peak = *std::max_element(out.begin(), out.end());
    if (peak > 0.0)
        for (auto& x : out)
            x /= peak;
    return out;
}

// The boundaries (bar indices, 0 first) by DP: phrases of 8 / 16 / 24 / 32 bars, 4 at a cost; the last one free.
std::vector<int> segment(const std::vector<double>& nov, const PhraseParams& p)
{
    const int n = static_cast<int>(nov.size()); // bars
    constexpr double kNone = -1e18;
    std::vector<double> best(static_cast<std::size_t>(n + 1), kNone);
    std::vector<int> from(static_cast<std::size_t>(n + 1), -1);
    best[0] = 0.0;
    static constexpr std::array<int, 5> kLengths{4, 8, 16, 24, 32};
    for (int b = 1; b <= n; ++b)
    {
        const bool end = b == n;
        // Scores of a boundary at bar b (none at the end).
        const double gain = end ? 0.0 : nov[static_cast<std::size_t>(b)] - p.threshold + (b % 8 == 0 ? p.gridBonus : 0.0);
        for (const int len : kLengths)
        {
            const int a = b - len;
            if (a < 0 || best[static_cast<std::size_t>(a)] == kNone)
                continue;
            const double score = best[static_cast<std::size_t>(a)] + gain - (len == 4 ? p.shortPenalty : 0.0);
            if (score > best[static_cast<std::size_t>(b)])
            {
                best[static_cast<std::size_t>(b)] = score;
                from[static_cast<std::size_t>(b)] = a;
            }
        }
        if (end) // the last phrase: whatever is left after any boundary (up to 32 bars, else it must split)
            for (int a = std::max(0, b - 32); a < b; ++a)
                if (best[static_cast<std::size_t>(a)] != kNone && best[static_cast<std::size_t>(a)] > best[static_cast<std::size_t>(b)])
                {
                    best[static_cast<std::size_t>(b)] = best[static_cast<std::size_t>(a)];
                    from[static_cast<std::size_t>(b)] = a;
                }
    }
    std::vector<int> starts;
    if (best[static_cast<std::size_t>(n)] == kNone) // shorter than any phrase: one
        return {0};
    for (int b = from[static_cast<std::size_t>(n)]; b > 0; b = from[static_cast<std::size_t>(b)])
        starts.push_back(b);
    starts.push_back(0);
    std::reverse(starts.begin(), starts.end());
    return starts;
}
} // namespace

PhraseAnalysis detectPhrases(const float* mono, int numSamples, double sampleRate, const util::Beatgrid& grid,
                             const PhraseParams& params)
{
    PhraseAnalysis out;
    if (mono == nullptr || numSamples <= 0 || sampleRate <= 0.0 || grid.empty())
        return out;
    // The bars: downbeats inside the audio (the grid's samples are at its own rate).
    const double gridRate = grid.sampleRate();
    const double duration = numSamples / sampleRate;
    auto beat = static_cast<long long>(std::ceil(grid.beatAt(0.0) - 1e-9));
    for (int k = 0; k < 8 && grid.barBeatOf(beat) != 1; ++k)
        ++beat;
    for (;; beat += util::Beatgrid::kBeatsPerBar)
    {
        const double t = grid.sampleAt(static_cast<double>(beat)) / gridRate;
        if (t > duration)
            break;
        out.barStarts.push_back(t);
        if (out.barStarts.size() > 100000)
            break;
    }
    if (out.barStarts.size() < 3)
        return out;
    // The last bar boundary: the end of the audio (a part-bar tail joins the last bar).
    std::vector<double> edges = out.barStarts;
    if (duration - edges.back() < 0.5 * (edges.back() - edges[edges.size() - 2]))
        edges.back() = duration;
    else
        edges.push_back(duration);
    out.barStarts.assign(edges.begin(), edges.end() - 1);

    const auto features = barFeatures(mono, numSamples, sampleRate, edges);
    out.novelty = novelty(vectors(features), params.kernelBars);
    const auto starts = segment(out.novelty, params);

    // Energy: loudness against the loudest phrase (18 dB down = 0) and onset density against the busiest.
    struct Raw
    {
        double db, flux;
    };
    std::vector<Raw> raw;
    for (std::size_t i = 0; i < starts.size(); ++i)
    {
        const int a = starts[i];
        const int b = i + 1 < starts.size() ? starts[i + 1] : static_cast<int>(features.size());
        double power = 0.0, flux = 0.0;
        for (int k = a; k < b; ++k)
        {
            power += std::pow(10.0, features[static_cast<std::size_t>(k)].levelDb / 10.0);
            flux += features[static_cast<std::size_t>(k)].flux;
        }
        raw.push_back({toDb(power / std::max(1, b - a)), flux / std::max(1, b - a)});
        out.phrases.push_back({out.barStarts[static_cast<std::size_t>(a)], b - a, 0.0});
    }
    double loudest = -200.0, busiest = 0.0;
    for (const auto& r : raw)
    {
        loudest = std::max(loudest, r.db);
        busiest = std::max(busiest, r.flux);
    }
    for (std::size_t i = 0; i < raw.size(); ++i)
    {
        const double loud = std::clamp(1.0 + (raw[i].db - loudest) / 18.0, 0.0, 1.0);
        const double busy = busiest > 0.0 ? raw[i].flux / busiest : 0.0;
        out.phrases[i].energy = std::clamp(0.7 * loud + 0.3 * busy, 0.0, 1.0);
    }
    return out;
}

BoundaryScore scoreBoundaries(const std::vector<double>& detected, const std::vector<double>& reference,
                              double tolerance)
{
    BoundaryScore s;
    s.detected = static_cast<int>(detected.size());
    s.reference = static_cast<int>(reference.size());
    std::vector<bool> used(reference.size(), false);
    for (const double d : detected)
    {
        int bestIndex = -1;
        double bestGap = tolerance;
        for (std::size_t r = 0; r < reference.size(); ++r)
            if (!used[r] && std::abs(reference[r] - d) <= bestGap)
            {
                bestGap = std::abs(reference[r] - d);
                bestIndex = static_cast<int>(r);
            }
        if (bestIndex >= 0)
        {
            used[static_cast<std::size_t>(bestIndex)] = true;
            ++s.hits;
        }
    }
    return s;
}

} // namespace devildj::analysis
