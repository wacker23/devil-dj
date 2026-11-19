#include "BeatTracker.h"

#include "analysis/Fft.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <numeric>

namespace devildj::analysis
{

namespace
{
constexpr int kFrame = 1024;
constexpr int kHop = 128;
constexpr double kLowBandHz = 200.0;
constexpr double kLogCompression = 100.0;
// The log-compressed flux reacts as soon as a sound enters the edge of the (centred) analysis window, so it marks an
// onset ~15.5 ms before the sound starts. Measured on the generated click tracks (all five tempos the same) and
// added to every beat time.
constexpr double kOnsetLatencySeconds = -0.0155;

// Removes the local mean (~0.3 s), keeps the positive part and scales to unit standard deviation.
void normalize(std::vector<float>& x, double frameRate)
{
    const int half = std::max(1, static_cast<int>(0.15 * frameRate));
    const int n = static_cast<int>(x.size());
    std::vector<double> prefix(static_cast<std::size_t>(n) + 1, 0.0);
    for (int i = 0; i < n; ++i)
        prefix[static_cast<std::size_t>(i) + 1] = prefix[static_cast<std::size_t>(i)] + x[static_cast<std::size_t>(i)];
    std::vector<float> out(x.size());
    for (int i = 0; i < n; ++i)
    {
        const int a = std::max(0, i - half), b = std::min(n, i + half + 1);
        const double mean = (prefix[static_cast<std::size_t>(b)] - prefix[static_cast<std::size_t>(a)]) / (b - a);
        out[static_cast<std::size_t>(i)] = static_cast<float>(std::max(0.0, x[static_cast<std::size_t>(i)] - mean));
    }
    double sum = 0.0, sq = 0.0;
    for (const float v : out)
    {
        sum += v;
        sq += static_cast<double>(v) * v;
    }
    const double mean = n > 0 ? sum / n : 0.0;
    const double sd = n > 0 ? std::sqrt(std::max(0.0, sq / n - mean * mean)) : 0.0;
    if (sd > 1e-12)
        for (auto& v : out)
            v = static_cast<float>(v / sd);
    x.swap(out);
}

// Autocorrelation of the (mean-removed) envelope for lags 0..maxLag.
std::vector<double> autocorrelation(const std::vector<float>& x, int maxLag)
{
    const int n = static_cast<int>(x.size());
    const double mean = n > 0 ? std::accumulate(x.begin(), x.end(), 0.0) / n : 0.0;
    std::vector<double> r(static_cast<std::size_t>(maxLag) + 1, 0.0);
    for (int lag = 0; lag <= maxLag && lag < n; ++lag)
    {
        double s = 0.0;
        for (int i = lag; i < n; ++i)
            s += (x[static_cast<std::size_t>(i)] - mean) * (x[static_cast<std::size_t>(i - lag)] - mean);
        r[static_cast<std::size_t>(lag)] = s / (n - lag);
    }
    const double r0 = r[0] > 0 ? r[0] : 1.0;
    for (auto& v : r)
        v /= r0;
    return r;
}

double at(const std::vector<double>& r, double lag)
{
    if (lag < 0.0 || lag >= static_cast<double>(r.size() - 1))
        return 0.0;
    const auto i = static_cast<std::size_t>(lag);
    const double f = lag - static_cast<double>(i);
    return r[i] * (1.0 - f) + r[i + 1] * f;
}

// Sub-frame position of a peak at frame i (parabola through i-1, i, i+1).
double refine(const std::vector<float>& o, int i)
{
    if (i <= 0 || i + 1 >= static_cast<int>(o.size()))
        return i;
    const double a = o[static_cast<std::size_t>(i - 1)], b = o[static_cast<std::size_t>(i)],
                 c = o[static_cast<std::size_t>(i + 1)];
    const double d = a - 2.0 * b + c;
    if (d >= 0.0)
        return i;
    return i + std::clamp(0.5 * (a - c) / d, -0.5, 0.5);
}

struct Fit
{
    double t0 = 0.0, period = 0.0;
};

// Least squares: time = t0 + k * period.
Fit fitLine(const std::vector<double>& k, const std::vector<double>& t)
{
    const double n = static_cast<double>(k.size());
    const double mk = std::accumulate(k.begin(), k.end(), 0.0) / n;
    const double mt = std::accumulate(t.begin(), t.end(), 0.0) / n;
    double skk = 0.0, skt = 0.0;
    for (std::size_t i = 0; i < k.size(); ++i)
    {
        skk += (k[i] - mk) * (k[i] - mk);
        skt += (k[i] - mk) * (t[i] - mt);
    }
    Fit f;
    f.period = skk > 0 ? skt / skk : 0.0;
    f.t0 = mt - f.period * mk;
    return f;
}
} // namespace

OnsetEnvelope onsetEnvelope(const float* mono, int numSamples, double sampleRate)
{
    OnsetEnvelope env;
    env.frameRate = sampleRate / kHop;
    // Frame i covers samples [i * hop - frame / 2, i * hop + frame / 2): its time is i * hop.
    env.frameOffsetSeconds = -kOnsetLatencySeconds;
    if (numSamples <= 0)
        return env;
    const int frames = numSamples / kHop + 1;
    const Fft fft(kFrame);
    std::vector<float> window(kFrame), frame(kFrame);
    for (int i = 0; i < kFrame; ++i)
        window[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kFrame));
    const int lowBins = std::max(2, static_cast<int>(kLowBandHz / (sampleRate / kFrame)));

    std::vector<double> mag, previous(kFrame / 2 + 1, 0.0);
    std::vector<std::complex<double>> work;
    env.flux.resize(static_cast<std::size_t>(frames));
    env.lowFlux.resize(static_cast<std::size_t>(frames));
    for (int f = 0; f < frames; ++f)
    {
        const int start = f * kHop - kFrame / 2;
        for (int i = 0; i < kFrame; ++i)
        {
            const int s = start + i;
            frame[static_cast<std::size_t>(i)] =
                s >= 0 && s < numSamples ? mono[s] * window[static_cast<std::size_t>(i)] : 0.0f;
        }
        fft.magnitudes(frame.data(), mag, work);
        double all = 0.0, low = 0.0;
        for (std::size_t k = 1; k < mag.size(); ++k)
        {
            const double l = std::log1p(kLogCompression * mag[k]);
            const double rise = l - previous[k];
            if (rise > 0.0)
            {
                all += rise;
                if (static_cast<int>(k) <= lowBins)
                    low += rise;
            }
            previous[k] = l;
        }
        env.flux[static_cast<std::size_t>(f)] = static_cast<float>(all);
        env.lowFlux[static_cast<std::size_t>(f)] = static_cast<float>(low);
    }
    normalize(env.flux, env.frameRate);
    normalize(env.lowFlux, env.frameRate);
    return env;
}

BeatGrid trackBeats(const float* mono, int numSamples, double sampleRate, const BeatTrackerParams& p)
{
    BeatGrid grid;
    const auto env = onsetEnvelope(mono, numSamples, sampleRate);
    const auto& o = env.flux;
    const double fr = env.frameRate;
    if (o.size() < static_cast<std::size_t>(fr * 8.0)) // under ~8 s: not enough to be sure
        return grid;

    // 1. Tempo: comb filter over the autocorrelation (the beat lag and its multiples), a prior for the
    //    preferred range, then octave checks.
    const int maxLag = static_cast<int>(std::ceil(4.0 * fr * 60.0 / p.minBpm)) + 2;
    const auto r = autocorrelation(o, maxLag);
    const auto comb = [&](double bpm)
    {
        const double lag = fr * 60.0 / bpm;
        return at(r, lag) + 0.5 * at(r, 2 * lag) + 0.33 * at(r, 3 * lag) + 0.25 * at(r, 4 * lag);
    };
    const auto prior = [&](double bpm)
    {
        if (bpm < p.preferMin)
            return std::exp(-0.5 * std::pow((p.preferMin - bpm) / 8.0, 2.0));
        if (bpm > p.preferMax)
            return std::exp(-0.5 * std::pow((bpm - p.preferMax) / 8.0, 2.0));
        return 1.0;
    };
    const auto peakNear = [&](double bpm, double width)
    {
        double best = bpm, bestScore = -1e9;
        for (double b = bpm * (1.0 - width); b <= bpm * (1.0 + width); b += 0.01)
            if (b >= p.minBpm && b <= p.maxBpm)
                if (const double s = comb(b); s > bestScore)
                {
                    bestScore = s;
                    best = b;
                }
        return std::pair{best, bestScore};
    };
    double bestBpm = 0.0, bestScore = -1e9, meanScore = 0.0;
    int count = 0;
    for (double bpm = p.minBpm; bpm <= p.maxBpm; bpm += 0.02)
    {
        const double s = comb(bpm);
        meanScore += s;
        ++count;
        if (s * prior(bpm) > bestScore)
        {
            bestScore = s * prior(bpm);
            bestBpm = bpm;
        }
    }
    meanScore /= std::max(1, count);
    const double rawBest = comb(bestBpm);
    // Octaves: the faster of two tempos that both fit the preferred range (a pulse at 2x explains the slower
    // one too); a tempo outside it halves / doubles into it.
    if (const auto [dbl, ds] = peakNear(bestBpm * 2.0, 0.01);
        dbl <= p.preferMax && dbl >= p.preferMin && ds >= 0.9 * rawBest)
        bestBpm = dbl;
    else if (bestBpm > p.preferMax)
    {
        if (const auto [half, hs] = peakNear(bestBpm / 2.0, 0.01); half >= p.preferMin && hs >= 0.5 * rawBest)
            bestBpm = half;
    }
    else if (bestBpm < p.preferMin)
    {
        if (const auto [dbl2, ds2] = peakNear(bestBpm * 2.0, 0.01); dbl2 <= p.preferMax && ds2 >= 0.5 * rawBest)
            bestBpm = dbl2;
    }
    grid.tempogramBpm = bestBpm;
    if (rawBest - meanScore < 0.05) // no periodicity to speak of
        return grid;

    // 2. Beats by dynamic programming: score(t) = onset(t) + best earlier beat about one period back.
    const double period = fr * 60.0 / bestBpm;
    const int n = static_cast<int>(o.size());
    std::vector<double> score(static_cast<std::size_t>(n), 0.0);
    std::vector<int> back(static_cast<std::size_t>(n), -1);
    constexpr double kTightness = 400.0;
    const int lo = static_cast<int>(std::round(period / 2.0)), hi = static_cast<int>(std::round(period * 2.0));
    for (int t = 0; t < n; ++t)
    {
        double best = 0.0;
        int from = -1;
        for (int tau = std::max(0, t - hi); tau <= t - lo; ++tau)
        {
            const double gap = std::log((t - tau) / period);
            const double s = score[static_cast<std::size_t>(tau)] - kTightness * gap * gap;
            if (from < 0 || s > best)
            {
                best = s;
                from = tau;
            }
        }
        score[static_cast<std::size_t>(t)] = o[static_cast<std::size_t>(t)] + (from >= 0 ? std::max(0.0, best) : 0.0);
        back[static_cast<std::size_t>(t)] = from >= 0 && best > 0.0 ? from : -1;
    }
    int last = n - 1;
    for (int t = std::max(0, n - static_cast<int>(period)); t < n; ++t)
        if (score[static_cast<std::size_t>(t)] > score[static_cast<std::size_t>(last)])
            last = t;
    std::vector<int> beatFrames;
    for (int t = last; t >= 0; t = back[static_cast<std::size_t>(t)])
        beatFrames.push_back(t);
    std::reverse(beatFrames.begin(), beatFrames.end());

    // Drop beats in silence at the start and the end (the chain runs on through them).
    std::vector<float> strengths;
    for (const int b : beatFrames)
        strengths.push_back(o[static_cast<std::size_t>(b)]);
    auto sorted = strengths;
    std::sort(sorted.begin(), sorted.end());
    const float median = sorted.empty() ? 0.0f : sorted[sorted.size() / 2];
    std::size_t first = 0, end = beatFrames.size();
    while (first < end && strengths[first] < 0.1f * median)
        ++first;
    while (end > first && strengths[end - 1] < 0.1f * median)
        --end;
    if (end - first < 8)
        return grid;

    // 3. Constant-tempo grid by least squares on the refined beat times; outliers (> 15 % of a period off) out.
    std::vector<double> times, index;
    double k = 0.0;
    for (std::size_t i = first; i < end; ++i)
    {
        const double frameTime = refine(o, beatFrames[i]);
        if (!times.empty())
            k += std::max(1.0, std::round((frameTime / fr + env.frameOffsetSeconds - times.back()) * fr / period));
        times.push_back(frameTime / fr + env.frameOffsetSeconds);
        index.push_back(k);
    }
    Fit fit = fitLine(index, times);
    for (int pass = 0; pass < 3 && fit.period > 0.0; ++pass)
    {
        std::vector<double> keptK, keptT;
        for (std::size_t i = 0; i < times.size(); ++i)
            if (std::abs(times[i] - (fit.t0 + index[i] * fit.period)) < 0.15 * fit.period)
            {
                keptK.push_back(index[i]);
                keptT.push_back(times[i]);
            }
        if (keptK.size() < 8)
            break;
        index.swap(keptK);
        times.swap(keptT);
        fit = fitLine(index, times);
    }
    if (fit.period <= 0.0)
        return grid;
    grid.valid = true;
    grid.bpm = 60.0 / fit.period;
    grid.beatsUsed = static_cast<int>(index.size());
    const double firstK = index.front();
    grid.firstBeatSeconds = fit.t0 + firstK * fit.period;

    // 4. Downbeat: the phase of 4 with the strongest onsets on its beats (low band counts double).
    double phaseScore[4] = {};
    int phaseCount[4] = {};
    for (double kk = 0.0; kk <= index.back() - firstK; kk += 1.0)
    {
        const double t = grid.firstBeatSeconds + kk * fit.period;
        const int frame = static_cast<int>(std::lround((t - env.frameOffsetSeconds) * fr));
        if (frame < 0 || frame >= n)
            continue;
        float peak = 0.0f, lowPeak = 0.0f;
        for (int d = -2; d <= 2; ++d)
            if (frame + d >= 0 && frame + d < n)
            {
                peak = std::max(peak, o[static_cast<std::size_t>(frame + d)]);
                lowPeak = std::max(lowPeak, env.lowFlux[static_cast<std::size_t>(frame + d)]);
            }
        const int phase = static_cast<int>(kk) % 4;
        phaseScore[phase] += peak + 2.0 * lowPeak;
        ++phaseCount[phase];
    }
    int downbeatPhase = 0;
    for (int ph = 1; ph < 4; ++ph)
        if (phaseCount[ph] > 0 && phaseScore[ph] / phaseCount[ph] > phaseScore[downbeatPhase] / std::max(1, phaseCount[downbeatPhase]))
            downbeatPhase = ph;
    grid.firstBeatInBar = (4 - downbeatPhase) % 4 + 1;

    // 5. Drift: local tempo over windows of 16 beats.
    for (std::size_t start = 0; start + 16 <= index.size(); start += 8)
    {
        const std::vector<double> wk(index.begin() + static_cast<std::ptrdiff_t>(start),
                                     index.begin() + static_cast<std::ptrdiff_t>(start + 16));
        const std::vector<double> wt(times.begin() + static_cast<std::ptrdiff_t>(start),
                                     times.begin() + static_cast<std::ptrdiff_t>(start + 16));
        const auto local = fitLine(wk, wt);
        if (local.period > 0.0)
            grid.maxDriftPercent = std::max(grid.maxDriftPercent, std::abs(local.period / fit.period - 1.0) * 100.0);
    }
    grid.variableTempo = grid.maxDriftPercent > 1.0;
    return grid;
}

std::string beatgridJson(const BeatGrid& grid, double trackSampleRate)
{
    if (!grid.valid)
        return "[]";
    char buf[200];
    std::snprintf(buf, sizeof(buf), "[{\"beat\":0,\"sample\":%lld,\"bpm\":%.4f,\"bar_beat\":%d}]",
                  static_cast<long long>(std::llround(grid.firstBeatSeconds * trackSampleRate)), grid.bpm,
                  grid.firstBeatInBar);
    return buf;
}

} // namespace devildj::analysis
