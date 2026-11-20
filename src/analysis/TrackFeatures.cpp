#include "TrackFeatures.h"

#include "analysis/Fft.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace devildj::analysis
{

namespace
{
constexpr int kFrame = 1024;
constexpr int kHop = 256;
constexpr double kLowHz = 150.0, kMidHz = 2500.0, kHatHz = 5000.0;
constexpr int kMelBands = 26, kMfccs = 13;
constexpr double kMelLowHz = 20.0, kMelHighHz = 8000.0;
constexpr double kSilentPower = 1e-8; // mean square under -80 dBFS: not used for the spectral averages
constexpr int kBeatsPerWindow = 64;  // 16 bars of 4
constexpr double kOnsetFloor = 0.5;  // summed log-magnitude rises: a soft click 40 dB down is still well over it
constexpr double kWindowSecondsNoGrid = 30.0;

struct Frame
{
    double power = 0.0; // mean square of the samples
    double low = 0.0, mid = 0.0, high = 0.0, hats = 0.0, total = 0.0; // spectral power per band
    double centroid = 0.0, rolloff = 0.0, flatness = 0.0;
    std::array<double, kMfccs> mfcc{};
    double flux = 0.0, fluxLow = 0.0, fluxMid = 0.0, fluxHigh = 0.0;
    bool silent = true;
};

double hzToMel(double hz)
{
    return 2595.0 * std::log10(1.0 + hz / 700.0);
}
double melToHz(double mel)
{
    return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
}

// Triangular mel filters over the FFT bins, each kept as the bins it covers (first bin + weights).
struct MelFilter
{
    int first = 0;
    std::vector<double> weights;
};

std::vector<MelFilter> melFilters(double rate)
{
    const int bins = kFrame / 2 + 1;
    std::vector<MelFilter> filters(kMelBands);
    const double lo = hzToMel(kMelLowHz), hi = hzToMel(std::min(kMelHighHz, rate / 2.0));
    std::vector<double> edges(kMelBands + 2);
    for (int i = 0; i < kMelBands + 2; ++i)
        edges[static_cast<std::size_t>(i)] = melToHz(lo + (hi - lo) * i / (kMelBands + 1));
    for (int m = 0; m < kMelBands; ++m)
    {
        const double a = edges[static_cast<std::size_t>(m)], b = edges[static_cast<std::size_t>(m + 1)],
                     c = edges[static_cast<std::size_t>(m + 2)];
        auto& filter = filters[static_cast<std::size_t>(m)];
        filter.first = -1;
        for (int k = 1; k < bins; ++k)
        {
            const double f = k * rate / kFrame;
            double v = 0.0;
            if (f > a && f <= b)
                v = (f - a) / (b - a);
            else if (f > b && f < c)
                v = (c - f) / (c - b);
            if (v <= 0.0)
                continue;
            if (filter.first < 0)
                filter.first = k;
            filter.weights.resize(static_cast<std::size_t>(k - filter.first + 1), 0.0);
            filter.weights.back() = v;
        }
        if (filter.first < 0)
            filter.first = 0;
    }
    return filters;
}

std::vector<Frame> analyzeFrames(const float* x, int n, double rate)
{
    const int frames = n > 0 ? n / kHop + 1 : 0;
    std::vector<Frame> out(static_cast<std::size_t>(frames));
    const Fft fft(kFrame);
    std::vector<float> window(kFrame), buf(kFrame);
    for (int i = 0; i < kFrame; ++i)
        window[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kFrame));
    const auto mel = melFilters(rate);
    const int bins = kFrame / 2 + 1;
    std::vector<double> mag, previous(static_cast<std::size_t>(bins), 0.0), melEnergy(kMelBands);
    std::vector<std::complex<double>> work;
    const double binHz = rate / kFrame;

    for (int f = 0; f < frames; ++f)
    {
        auto& fr = out[static_cast<std::size_t>(f)];
        const int start = f * kHop - kFrame / 2;
        double sq = 0.0;
        for (int i = 0; i < kFrame; ++i)
        {
            const int s = start + i;
            const float v = s >= 0 && s < n ? x[s] : 0.0f;
            sq += static_cast<double>(v) * v;
            buf[static_cast<std::size_t>(i)] = v * window[static_cast<std::size_t>(i)];
        }
        fr.power = sq / kFrame;
        fr.silent = fr.power < kSilentPower;
        fft.magnitudes(buf.data(), mag, work);

        double weighted = 0.0, logSum = 0.0;
        for (int k = 1; k < bins; ++k)
        {
            const double m = mag[static_cast<std::size_t>(k)];
            const double p = m * m;
            const double hz = k * binHz;
            fr.total += p;
            (hz < kLowHz ? fr.low : hz < kMidHz ? fr.mid : fr.high) += p;
            if (hz >= kHatHz)
                fr.hats += p;
            weighted += hz * p;
            logSum += std::log(p + 1e-12);
            // Spectral flux (log-compressed rises), per band.
            const double l = std::log1p(100.0 * m);
            const double rise = l - previous[static_cast<std::size_t>(k)];
            previous[static_cast<std::size_t>(k)] = l;
            if (rise > 0.0)
            {
                fr.flux += rise;
                if (hz < kLowHz)
                    fr.fluxLow += rise;
                else if (hz < kMidHz)
                    fr.fluxMid += rise;
                else if (hz >= kHatHz)
                    fr.fluxHigh += rise;
            }
        }
        if (fr.total <= 0.0)
            continue;
        fr.centroid = weighted / fr.total;
        fr.flatness = std::exp(logSum / (bins - 1)) / (fr.total / (bins - 1) + 1e-12);
        double cumulative = 0.0;
        for (int k = 1; k < bins; ++k)
        {
            cumulative += mag[static_cast<std::size_t>(k)] * mag[static_cast<std::size_t>(k)];
            if (cumulative >= 0.85 * fr.total)
            {
                fr.rolloff = k * binHz;
                break;
            }
        }
        // MFCC 1..13: DCT-II of the log mel energies.
        for (int m = 0; m < kMelBands; ++m)
        {
            const auto& filter = mel[static_cast<std::size_t>(m)];
            double e = 0.0;
            for (std::size_t j = 0; j < filter.weights.size(); ++j)
            {
                const double v = mag[static_cast<std::size_t>(filter.first) + j];
                e += filter.weights[j] * v * v;
            }
            melEnergy[static_cast<std::size_t>(m)] = std::log(e + 1e-10);
        }
        for (int c = 1; c <= kMfccs; ++c)
        {
            double v = 0.0;
            for (int m = 0; m < kMelBands; ++m)
                v += melEnergy[static_cast<std::size_t>(m)] * std::cos(std::numbers::pi * c * (m + 0.5) / kMelBands);
            fr.mfcc[static_cast<std::size_t>(c - 1)] = v;
        }
    }
    return out;
}

// Onsets: peaks of the full-band flux at least 3 x its local median (+-16 frames) - an attack stands far above
// the flux between attacks, a held tone's wobble doesn't - and over an absolute floor, at least 3 frames (~35 ms)
// apart.
std::vector<int> onsetFrames(const std::vector<Frame>& frames)
{
    const int n = static_cast<int>(frames.size());
    std::vector<int> out;
    std::vector<double> around;
    for (int i = 1; i + 1 < n; ++i)
    {
        const double v = frames[static_cast<std::size_t>(i)].flux;
        if (v <= kOnsetFloor || v <= frames[static_cast<std::size_t>(i - 1)].flux ||
            v < frames[static_cast<std::size_t>(i + 1)].flux)
            continue;
        around.clear();
        for (int j = std::max(0, i - 16); j < std::min(n, i + 17); ++j)
            around.push_back(frames[static_cast<std::size_t>(j)].flux);
        const auto mid = around.begin() + static_cast<std::ptrdiff_t>(around.size() / 2);
        std::nth_element(around.begin(), mid, around.end());
        if (v > 3.0 * *mid && (out.empty() || i - out.back() >= 3))
            out.push_back(i);
    }
    return out;
}
} // namespace

TrackFeatures computeTrackFeatures(const float* mono, int numSamples, double sampleRate, const util::Beatgrid& grid)
{
    TrackFeatures out;
    if (mono == nullptr || numSamples < kFrame || sampleRate <= 0.0)
        return out;
    const auto frames = analyzeFrames(mono, numSamples, sampleRate);
    const auto onsets = onsetFrames(frames);
    const double fps = sampleRate / kHop;
    const double duration = numSamples / sampleRate;

    // ---- energy windows: 16 bars of the grid, else 30 s ----
    std::vector<double> bounds; // seconds
    if (!grid.empty())
    {
        double first = std::ceil(grid.beatAt(0.0) - 1e-9);
        for (double b = first;; b += kBeatsPerWindow)
        {
            const double t = grid.sampleAt(b) / sampleRate;
            if (t >= duration)
                break;
            bounds.push_back(std::max(0.0, t));
        }
    }
    else
    {
        for (double t = 0.0; t < duration; t += kWindowSecondsNoGrid)
            bounds.push_back(t);
    }
    bounds.push_back(duration);
    // A short last window joins the one before (under a quarter of a window).
    if (bounds.size() > 2)
    {
        const double full = bounds[1] - bounds[0];
        if (bounds[bounds.size() - 1] - bounds[bounds.size() - 2] < 0.25 * full)
            bounds.erase(bounds.end() - 2);
    }
    std::size_t nextOnset = 0;
    for (std::size_t w = 0; w + 1 < bounds.size(); ++w)
    {
        const double from = bounds[w], to = bounds[w + 1];
        if (to - from <= 0.0)
            continue;
        const int fa = std::clamp(static_cast<int>(std::ceil(from * fps)), 0, static_cast<int>(frames.size()));
        const int fb = std::clamp(static_cast<int>(std::ceil(to * fps)), fa, static_cast<int>(frames.size()));
        double power = 0.0, low = 0.0, total = 0.0, centroid = 0.0, centroidWeight = 0.0;
        for (int f = fa; f < fb; ++f)
        {
            const auto& fr = frames[static_cast<std::size_t>(f)];
            power += fr.power;
            low += fr.low;
            total += fr.total;
            if (!fr.silent)
            {
                centroid += fr.centroid * fr.total;
                centroidWeight += fr.total;
            }
        }
        int count = 0;
        while (nextOnset < onsets.size() && onsets[nextOnset] < fb)
        {
            if (onsets[nextOnset] >= fa)
                ++count;
            ++nextOnset;
        }
        EnergyWindow e;
        e.startSeconds = static_cast<float>(from);
        e.seconds = static_cast<float>(to - from);
        const double meanPower = fb > fa ? power / (fb - fa) : 0.0;
        e.loudnessDb = static_cast<float>(meanPower > 1e-10 ? 10.0 * std::log10(meanPower) : -100.0);
        e.onsetRate = static_cast<float>(count / (to - from));
        e.lowRatio = static_cast<float>(total > 0.0 ? low / total : 0.0);
        e.centroidHz = static_cast<float>(centroidWeight > 0.0 ? centroid / centroidWeight : 0.0);
        out.windows.push_back(e);
    }

    // ---- sound vector ----
    {
        std::array<double, kMfccs> sum{}, sumSq{};
        double centroid = 0.0, rolloff = 0.0, flatness = 0.0, low = 0.0, high = 0.0, total = 0.0;
        int used = 0;
        for (const auto& fr : frames)
        {
            if (fr.silent)
                continue;
            ++used;
            for (int c = 0; c < kMfccs; ++c)
            {
                sum[static_cast<std::size_t>(c)] += fr.mfcc[static_cast<std::size_t>(c)];
                sumSq[static_cast<std::size_t>(c)] += fr.mfcc[static_cast<std::size_t>(c)] * fr.mfcc[static_cast<std::size_t>(c)];
            }
            centroid += std::log2(std::max(20.0, fr.centroid));
            rolloff += std::log2(std::max(20.0, fr.rolloff));
            flatness += 10.0 * std::log10(fr.flatness + 1e-12);
            low += fr.low;
            high += fr.high;
            total += fr.total;
        }
        if (used > 0)
        {
            std::array<float, kSoundDims> v{};
            for (int c = 0; c < kMfccs; ++c)
            {
                const double mean = sum[static_cast<std::size_t>(c)] / used;
                v[static_cast<std::size_t>(c)] = static_cast<float>(mean);
                v[static_cast<std::size_t>(kMfccs + c)] =
                    static_cast<float>(std::sqrt(std::max(0.0, sumSq[static_cast<std::size_t>(c)] / used - mean * mean)));
            }
            v[26] = static_cast<float>(centroid / used);
            v[27] = static_cast<float>(rolloff / used);
            v[28] = static_cast<float>(flatness / used);
            v[29] = static_cast<float>(onsets.size() / duration);
            v[30] = static_cast<float>(total > 0.0 ? low / total : 0.0);
            v[31] = static_cast<float>(total > 0.0 ? high / total : 0.0);
            out.sound = v;
        }
    }

    // ---- groove: accents on the 16 sixteenths of a bar, three bands ----
    if (!grid.empty())
    {
        std::array<double, kGrooveDims> acc{};
        int bars = 0;
        const auto fluxAt = [&](double sample, int band)
        {
            const int f = static_cast<int>(std::lround(sample / kHop));
            double peak = 0.0;
            for (int d = -1; d <= 1; ++d)
            {
                const int i = f + d;
                if (i < 0 || i >= static_cast<int>(frames.size()))
                    continue;
                const auto& fr = frames[static_cast<std::size_t>(i)];
                peak = std::max(peak, band == 0 ? fr.fluxLow : band == 1 ? fr.fluxMid : fr.fluxHigh);
            }
            return peak;
        };
        const auto firstBeat = static_cast<long long>(std::ceil(grid.beatAt(0.0) - 1e-9));
        for (long long b = firstBeat;; ++b)
        {
            const double start = grid.sampleAt(static_cast<double>(b));
            if (grid.sampleAt(static_cast<double>(b) + 4.0) >= numSamples)
                break;
            if (start < 0.0 || grid.barBeatOf(b) != 1)
                continue;
            for (int p = 0; p < 16; ++p)
            {
                const double at = grid.sampleAt(static_cast<double>(b) + p / 4.0);
                for (int band = 0; band < 3; ++band)
                    acc[static_cast<std::size_t>(band * 16 + p)] += fluxAt(at, band);
            }
            ++bars;
        }
        if (bars >= 4)
        {
            std::array<float, kGrooveDims> g{};
            double totalNorm = 0.0;
            for (int band = 0; band < 3; ++band)
            {
                double mean = 0.0;
                for (int p = 0; p < 16; ++p)
                    mean += acc[static_cast<std::size_t>(band * 16 + p)] / bars;
                mean /= 16.0;
                double norm = 0.0;
                for (int p = 0; p < 16; ++p)
                {
                    const double v = acc[static_cast<std::size_t>(band * 16 + p)] / bars - mean;
                    norm += v * v;
                }
                norm = std::sqrt(norm);
                for (int p = 0; p < 16; ++p)
                {
                    const double v = acc[static_cast<std::size_t>(band * 16 + p)] / bars - mean;
                    g[static_cast<std::size_t>(band * 16 + p)] = static_cast<float>(norm > 1e-9 ? v / norm : 0.0);
                }
                totalNorm += norm > 1e-9 ? 1.0 : 0.0;
            }
            if (totalNorm > 0.0)
            {
                for (auto& v : g)
                    v = static_cast<float>(v / std::sqrt(totalNorm));
                out.groove = g;
            }
        }
    }
    return out;
}

EnergyStats energyStats(std::span<const EnergyWindow> windows)
{
    EnergyStats s;
    if (windows.empty())
        return s;
    const auto value = [](const EnergyWindow& w, int i) -> double
    {
        return i == 0 ? w.loudnessDb : i == 1 ? w.onsetRate : i == 2 ? w.lowRatio : w.centroidHz;
    };
    for (int i = 0; i < 4; ++i)
    {
        double sum = 0.0, sq = 0.0;
        for (const auto& w : windows)
        {
            sum += value(w, i);
            sq += value(w, i) * value(w, i);
        }
        const double n = static_cast<double>(windows.size());
        s.mean[static_cast<std::size_t>(i)] = sum / n;
        s.sd[static_cast<std::size_t>(i)] = std::max(1e-6, std::sqrt(std::max(0.0, sq / n - (sum / n) * (sum / n))));
    }
    return s;
}

double rawEnergy(const EnergyWindow& w, const EnergyStats& s)
{
    const auto z = [&](double v, int i) { return (v - s.mean[static_cast<std::size_t>(i)]) / s.sd[static_cast<std::size_t>(i)]; };
    return 0.35 * z(w.loudnessDb, 0) + 0.25 * z(w.onsetRate, 1) + 0.20 * z(w.lowRatio, 2) + 0.20 * z(w.centroidHz, 3);
}

double cosine(std::span<const float> a, std::span<const float> b)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
    {
        ab += static_cast<double>(a[i]) * b[i];
        aa += static_cast<double>(a[i]) * a[i];
        bb += static_cast<double>(b[i]) * b[i];
    }
    return aa > 0.0 && bb > 0.0 ? ab / std::sqrt(aa * bb) : 0.0;
}

std::vector<std::uint8_t> packFloats(std::span<const float> values)
{
    std::vector<std::uint8_t> out(values.size() * 4);
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &values[i], 4);
        for (int b = 0; b < 4; ++b)
            out[i * 4 + static_cast<std::size_t>(b)] = static_cast<std::uint8_t>((bits >> (8 * b)) & 0xff);
    }
    return out;
}

std::vector<float> unpackFloats(std::span<const std::uint8_t> bytes)
{
    std::vector<float> out(bytes.size() / 4);
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        std::uint32_t bits = 0;
        for (int b = 3; b >= 0; --b)
            bits = (bits << 8) | bytes[i * 4 + static_cast<std::size_t>(b)];
        std::memcpy(&out[i], &bits, 4);
    }
    return out;
}

std::vector<std::uint8_t> packWindows(std::span<const EnergyWindow> windows)
{
    std::vector<float> flat;
    for (const auto& w : windows)
        flat.insert(flat.end(), {w.startSeconds, w.seconds, w.loudnessDb, w.onsetRate, w.lowRatio, w.centroidHz});
    return packFloats(flat);
}

std::vector<EnergyWindow> unpackWindows(std::span<const std::uint8_t> bytes)
{
    const auto flat = unpackFloats(bytes);
    std::vector<EnergyWindow> out;
    for (std::size_t i = 0; i + 6 <= flat.size(); i += 6)
        out.push_back({flat[i], flat[i + 1], flat[i + 2], flat[i + 3], flat[i + 4], flat[i + 5]});
    return out;
}

std::uint16_t toHalf(float value)
{
    std::uint32_t f = 0;
    std::memcpy(&f, &value, 4);
    const std::uint32_t sign = (f >> 16) & 0x8000u;
    const int exponent = static_cast<int>((f >> 23) & 0xff) - 127 + 15;
    std::uint32_t mantissa = f & 0x7fffffu;
    if (((f >> 23) & 0xff) == 0xff) // inf / nan
        return static_cast<std::uint16_t>(sign | 0x7c00u | (mantissa != 0 ? 0x200u : 0u));
    if (exponent >= 31)
        return static_cast<std::uint16_t>(sign | 0x7c00u);
    if (exponent <= 0)
    {
        if (exponent < -10)
            return static_cast<std::uint16_t>(sign);
        mantissa |= 0x800000u;
        const int shift = 14 - exponent;
        std::uint32_t half = mantissa >> shift;
        if ((mantissa >> (shift - 1)) & 1u) // round half up
            ++half;
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = sign | (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13);
    if (mantissa & 0x1000u) // round
        ++half;
    return static_cast<std::uint16_t>(half);
}

float fromHalf(std::uint16_t h)
{
    const std::uint32_t sign = (static_cast<std::uint32_t>(h) & 0x8000u) << 16;
    std::uint32_t exponent = (h >> 10) & 0x1fu;
    std::uint32_t mantissa = h & 0x3ffu;
    std::uint32_t f = 0;
    if (exponent == 0)
    {
        if (mantissa == 0)
        {
            f = sign;
        }
        else
        {
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x400u) == 0)
            {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x3ffu;
            f = sign | (exponent << 23) | (mantissa << 13);
        }
    }
    else if (exponent == 31)
    {
        f = sign | 0x7f800000u | (mantissa << 13);
    }
    else
    {
        f = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }
    float out = 0.0f;
    std::memcpy(&out, &f, 4);
    return out;
}

std::vector<std::uint8_t> packHalves(std::span<const float> values)
{
    std::vector<std::uint8_t> out(values.size() * 2);
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        const auto h = toHalf(values[i]);
        out[i * 2] = static_cast<std::uint8_t>(h & 0xff);
        out[i * 2 + 1] = static_cast<std::uint8_t>(h >> 8);
    }
    return out;
}

std::vector<float> unpackHalves(std::span<const std::uint8_t> bytes)
{
    std::vector<float> out(bytes.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = fromHalf(static_cast<std::uint16_t>(bytes[i * 2] | (bytes[i * 2 + 1] << 8)));
    return out;
}

} // namespace devildj::analysis
