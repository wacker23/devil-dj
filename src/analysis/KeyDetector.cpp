#include "KeyDetector.h"

#include "analysis/Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace devildj::analysis
{

namespace
{
constexpr int kFftSize = 8192;
constexpr int kHop = 2048;
constexpr double kLowHz = 65.4;    // C2
constexpr double kHighHz = 2093.0; // C7
constexpr double kPeakFloor = 0.01; // peaks under -40 dB of the frame's loudest are ignored
constexpr double kSilentRms = 1e-3; // -60 dBFS: frames this quiet don't vote

// Krumhansl-Kessler probe-tone ratings (Krumhansl 1990), C as the tonic. Published experimental data.
constexpr std::array<double, 12> kMajorProfile = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr std::array<double, 12> kMinorProfile = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

struct Peak
{
    int frame;
    double semitones; // above A4 = 440 Hz (fractional)
    double magnitude;
};

double pearson(const std::array<double, 12>& x, const std::array<double, 12>& profile, int root)
{
    double mx = 0.0, my = 0.0;
    for (int i = 0; i < 12; ++i)
    {
        mx += x[static_cast<std::size_t>(i)];
        my += profile[static_cast<std::size_t>(i)];
    }
    mx /= 12.0;
    my /= 12.0;
    double sxy = 0.0, sxx = 0.0, syy = 0.0;
    for (int i = 0; i < 12; ++i)
    {
        // Pitch class i against the profile's degree (i - root).
        const double a = x[static_cast<std::size_t>(i)] - mx;
        const double b = profile[static_cast<std::size_t>((i - root + 12) % 12)] - my;
        sxy += a * b;
        sxx += a * a;
        syy += b * b;
    }
    return sxx > 0.0 && syy > 0.0 ? sxy / std::sqrt(sxx * syy) : 0.0;
}
} // namespace

KeyEstimate detectKey(const float* mono, int numSamples, double sampleRate)
{
    KeyEstimate out;
    if (mono == nullptr || numSamples < kFftSize || sampleRate <= 0.0)
        return out;

    // 1. Spectral peaks of every frame (Hann window), their frequency refined by a parabola through the
    // log-magnitudes of the peak bin and its neighbours.
    const Fft fft(kFftSize);
    std::vector<float> window(kFftSize), frame(kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        window[static_cast<std::size_t>(i)] =
            static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / (kFftSize - 1)));
    std::vector<double> mag;
    std::vector<std::complex<double>> work;
    std::vector<Peak> peaks;
    std::vector<double> frameWeight;
    const double binHz = sampleRate / kFftSize;
    const int lowBin = std::max(2, static_cast<int>(kLowHz / binHz));
    const int highBin = std::min(kFftSize / 2 - 2, static_cast<int>(kHighHz / binHz) + 1);

    for (int start = 0; start + kFftSize <= numSamples; start += kHop)
    {
        double energy = 0.0;
        for (int i = 0; i < kFftSize; ++i)
        {
            const float v = mono[start + i];
            energy += static_cast<double>(v) * v;
            frame[static_cast<std::size_t>(i)] = v * window[static_cast<std::size_t>(i)];
        }
        const double rms = std::sqrt(energy / kFftSize);
        const int f = static_cast<int>(frameWeight.size());
        frameWeight.push_back(rms > kSilentRms ? rms : 0.0);
        if (rms <= kSilentRms)
            continue;
        fft.magnitudes(frame.data(), mag, work);
        double loudest = 0.0;
        for (int k = lowBin; k <= highBin; ++k)
            loudest = std::max(loudest, mag[static_cast<std::size_t>(k)]);
        const double floor = loudest * kPeakFloor;
        for (int k = lowBin; k <= highBin; ++k)
        {
            const double m = mag[static_cast<std::size_t>(k)];
            if (m <= floor || m <= mag[static_cast<std::size_t>(k - 1)] || m < mag[static_cast<std::size_t>(k + 1)])
                continue;
            const double a = std::log(mag[static_cast<std::size_t>(k - 1)] + 1e-12);
            const double b = std::log(m + 1e-12);
            const double c = std::log(mag[static_cast<std::size_t>(k + 1)] + 1e-12);
            const double denom = a - 2.0 * b + c;
            const double offset = denom < 0.0 ? std::clamp(0.5 * (a - c) / denom, -0.5, 0.5) : 0.0;
            const double hz = (k + offset) * binHz;
            peaks.push_back({f, 12.0 * std::log2(hz / 440.0), m});
        }
    }
    if (peaks.empty())
        return out;

    // 2. Tuning: the magnitude-weighted circular mean of every peak's deviation from equal temperament (A = 440).
    double re = 0.0, im = 0.0;
    for (const auto& p : peaks)
    {
        const double phase = 2.0 * std::numbers::pi * (p.semitones - std::round(p.semitones));
        re += p.magnitude * std::cos(phase);
        im += p.magnitude * std::sin(phase);
    }
    const double tuning = std::atan2(im, re) / (2.0 * std::numbers::pi); // semitones, -0.5..0.5
    out.tuningCents = 100.0 * tuning;

    // 3. Each frame's chroma: the peaks in 36 bins per octave around the tuned semitones, folded to 12 (the
    // centre third of a semitone counts fully, the outer thirds half), normalized, then summed with the frame's loudness as weight.
    std::vector<std::array<double, 12>> frameChroma(frameWeight.size());
    for (const auto& p : peaks)
    {
        const double tuned = p.semitones - tuning;
        const double nearest = std::round(tuned);
        const double third = std::round((tuned - nearest) * 3.0); // -1, 0, +1 (tuned bins)
        const double weight = third == 0.0 ? 1.0 : 0.5;
        // A4 is pitch class 9.
        const int pc = ((static_cast<int>(nearest) + 9) % 12 + 12) % 12;
        frameChroma[static_cast<std::size_t>(p.frame)][static_cast<std::size_t>(pc)] += weight * p.magnitude;
    }
    std::array<double, 12> chroma{};
    for (std::size_t f = 0; f < frameChroma.size(); ++f)
    {
        double sum = 0.0;
        for (const double v : frameChroma[f])
            sum += v;
        if (sum <= 0.0 || frameWeight[f] <= 0.0)
            continue;
        for (int i = 0; i < 12; ++i)
            chroma[static_cast<std::size_t>(i)] += frameWeight[f] * frameChroma[f][static_cast<std::size_t>(i)] / sum;
    }
    double total = 0.0;
    for (const double v : chroma)
        total += v;
    if (total <= 0.0)
        return out;
    for (auto& v : chroma)
        v /= total;
    out.chroma = chroma;

    // 4. Correlate with the 24 key profiles.
    int best = 0;
    for (int root = 0; root < 12; ++root)
    {
        out.scores[static_cast<std::size_t>(root)] = pearson(chroma, kMajorProfile, root);
        out.scores[static_cast<std::size_t>(12 + root)] = pearson(chroma, kMinorProfile, root);
    }
    for (int k = 1; k < 24; ++k)
        if (out.scores[static_cast<std::size_t>(k)] > out.scores[static_cast<std::size_t>(best)])
            best = k;
    double second = -1.0;
    for (int k = 0; k < 24; ++k)
        if (k != best)
            second = std::max(second, out.scores[static_cast<std::size_t>(k)]);
    out.key = best;
    out.correlation = out.scores[static_cast<std::size_t>(best)];
    out.valid = out.correlation > 0.0;
    // Confidence: a clear key both fits its profile well and beats the runner-up (usually the relative or a
    // fifth away) by a margin; a 0.15 lead counts as fully clear.
    out.confidence = std::clamp(out.correlation, 0.0, 1.0) * std::clamp((out.correlation - second) / 0.15, 0.0, 1.0);
    if (!out.valid)
        out.key = -1;
    return out;
}

} // namespace devildj::analysis
