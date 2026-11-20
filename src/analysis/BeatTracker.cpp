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

namespace
{
// Beats by dynamic programming: score(t) = onset(t) + the best earlier beat about one period back.
std::vector<int> dynamicBeats(const std::vector<float>& o, double period, double tightness)
{
    const int n = static_cast<int>(o.size());
    std::vector<double> score(static_cast<std::size_t>(n), 0.0);
    std::vector<int> back(static_cast<std::size_t>(n), -1);
    const int lo = static_cast<int>(std::round(period / 2.0)), hi = static_cast<int>(std::round(period * 2.0));
    for (int t = 0; t < n; ++t)
    {
        double best = 0.0;
        int from = -1;
        for (int tau = std::max(0, t - hi); tau <= t - lo; ++tau)
        {
            const double gap = std::log((t - tau) / period);
            const double s = score[static_cast<std::size_t>(tau)] - tightness * gap * gap;
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
    std::vector<int> beats;
    for (int t = last; t >= 0; t = back[static_cast<std::size_t>(t)])
        beats.push_back(t);
    std::reverse(beats.begin(), beats.end());
    return beats;
}

// The strongest onset within +-2 frames (~12 ms) of a position.
float strengthAt(const std::vector<float>& o, double frame)
{
    const int f = static_cast<int>(std::lround(frame));
    float peak = 0.0f;
    for (int d = -2; d <= 2; ++d)
        if (f + d >= 0 && f + d < static_cast<int>(o.size()))
            peak = std::max(peak, o[static_cast<std::size_t>(f + d)]);
    return peak;
}

// The beat chain for one tempo, with the beats in silence at either end trimmed.
struct Chain
{
    std::vector<int> frames;
    std::size_t first = 0, end = 0;
};

// Tightness: how hard the chain holds to the period (400: steady music; a live band needs less).
Chain beatChain(const std::vector<float>& o, double period, double tightness = 400.0)
{
    Chain c;
    c.frames = dynamicBeats(o, period, tightness);
    std::vector<float> strengths;
    for (const int b : c.frames)
        strengths.push_back(o[static_cast<std::size_t>(b)]);
    auto sorted = strengths;
    std::sort(sorted.begin(), sorted.end());
    const float median = sorted.empty() ? 0.0f : sorted[sorted.size() / 2];
    c.end = c.frames.size();
    while (c.first < c.end && strengths[c.first] < 0.1f * median)
        ++c.first;
    while (c.end > c.first && strengths[c.end - 1] < 0.1f * median)
        --c.end;
    return c;
}

// Splits beats (index, time) into straight pieces: each anchor's tempo holds until the next, and every beat
// stays within `tolerance` seconds of its piece.
std::vector<BeatAnchor> segment(const std::vector<double>& k, const std::vector<double>& t, double tolerance)
{
    std::vector<BeatAnchor> out;
    const std::size_t n = k.size();
    std::size_t s = 0;
    while (s + 1 < n)
    {
        std::size_t best = s + 1;
        for (std::size_t e = s + 1; e < n; ++e)
        {
            const double slope = (t[e] - t[s]) / (k[e] - k[s]);
            bool fits = true;
            for (std::size_t j = s + 1; j < e && fits; ++j)
                fits = std::abs(t[j] - (t[s] + (k[j] - k[s]) * slope)) <= tolerance;
            if (fits)
                best = e;
            else if (e - best > 8) // look a few beats past a bad one before giving up
                break;
        }
        out.push_back({static_cast<int>(std::lround(k[s] - k.front())), t[s], 60.0 * (k[best] - k[s]) / (t[best] - t[s])});
        s = best;
    }
    return out;
}
} // namespace

namespace
{
// Steps 3-5 for one beat chain: the constant grid, steady / live, the tempo-following grid, the downbeat.
BeatGrid fitGrid(const OnsetEnvelope& env, double bestBpm, const Chain& chain, const BeatTrackerParams& p)
{
    BeatGrid grid;
    const auto& o = env.flux;
    const double fr = env.frameRate;
    grid.tempogramBpm = bestBpm;
    if (chain.end - chain.first < 8)
        return grid;
    const double period = fr * 60.0 / bestBpm;

    // 3. Beat times (refined) and their beat numbers.
    std::vector<double> times, index;
    double k = 0.0;
    for (std::size_t i = chain.first; i < chain.end; ++i)
    {
        const double t = refine(o, chain.frames[i]) / fr + env.frameOffsetSeconds;
        if (!times.empty())
            k += std::max(1.0, std::round((t - times.back()) * fr / period));
        times.push_back(t);
        index.push_back(k);
    }
    // A constant grid first: least squares over the beats, outliers (> 15 % of a period off the line) out.
    std::vector<double> constK = index, constT = times;
    Fit fit = fitLine(constK, constT);
    // The gate tightens pass by pass (15 % of a period, then 8 %, then 5 % but at least 20 ms), so a steady
    // track's grid ends up on the beats to the millisecond rather than on average.
    for (const double gate : {0.15, 0.15, 0.15, 0.08, 0.05})
    {
        if (fit.period <= 0.0)
            break;
        const double limit = std::max(gate * fit.period, gate < 0.1 ? 0.020 : 0.0);
        std::vector<double> keptK, keptT;
        for (std::size_t i = 0; i < constT.size(); ++i)
            if (std::abs(constT[i] - (fit.t0 + constK[i] * fit.period)) < limit)
            {
                keptK.push_back(constK[i]);
                keptT.push_back(constT[i]);
            }
        if (keptK.size() < 8)
            break;
        constK.swap(keptK);
        constT.swap(keptT);
        fit = fitLine(constK, constT);
    }
    if (fit.period <= 0.0)
        return grid;
    grid.constantFraction = static_cast<double>(constK.size()) / static_cast<double>(times.size());

    // A tempo-following grid too: the beats that fit their neighbours (8 beats either side; a live band drifts
    // by whole beats over a song, which one straight line calls outliers), split into straight pieces.
    std::vector<double> localK = index, localT = times;
    for (int pass = 0; pass < 2; ++pass)
    {
        std::vector<double> keptK, keptT;
        for (std::size_t i = 0; i < localT.size(); ++i)
        {
            const std::size_t a = i >= 8 ? i - 8 : 0, b = std::min(localT.size(), i + 9);
            const std::vector<double> wk(localK.begin() + static_cast<std::ptrdiff_t>(a),
                                         localK.begin() + static_cast<std::ptrdiff_t>(b));
            const std::vector<double> wt(localT.begin() + static_cast<std::ptrdiff_t>(a),
                                         localT.begin() + static_cast<std::ptrdiff_t>(b));
            const auto local = fitLine(wk, wt);
            if (local.period <= 0.0 || std::abs(localT[i] - (local.t0 + localK[i] * local.period)) < 0.15 * period)
            {
                keptK.push_back(localK[i]);
                keptT.push_back(localT[i]);
            }
        }
        if (keptK.size() < 8)
            break;
        localK.swap(keptK);
        localT.swap(keptT);
    }
    std::vector<BeatAnchor> anchors;
    for (double tolerance = 0.015;; tolerance *= 1.4)
        if (anchors = segment(localK, localT, tolerance); anchors.size() <= 16)
            break;

    // Steady or not: split the beats into straight pieces (15 ms) and see how many beats sit in pieces at one
    // tempo (within 0.3 % of the beat-weighted median). Programmed music shows one exact tempo with a few short odd
    // pieces where the beat chain slipped half a beat; a live band's pieces wander.
    {
        const auto pieces = segment(localK, localT, 0.015);
        const double last = localK.back() - localK.front();
        std::vector<std::pair<double, double>> tempos; // bpm, beats
        double total = 0.0;
        for (std::size_t a = 0; a < pieces.size(); ++a)
        {
            const double beats = (a + 1 < pieces.size() ? pieces[a + 1].beat : last) - pieces[a].beat;
            tempos.push_back({pieces[a].bpm, beats});
            total += beats;
        }
        auto byTempo = tempos;
        std::sort(byTempo.begin(), byTempo.end());
        double median = byTempo.empty() ? 0.0 : byTempo.front().first, acc = 0.0;
        for (const auto& [bpm, beats] : byTempo)
            if ((acc += beats) >= 0.5 * total)
            {
                median = bpm;
                break;
            }
        double steady = 0.0, longest = 0.0, anchorTime = 0.0;
        for (std::size_t a = 0; a < tempos.size(); ++a)
            if (std::abs(tempos[a].first / median - 1.0) < 0.003)
            {
                steady += tempos[a].second;
                if (tempos[a].second > longest)
                {
                    longest = tempos[a].second;
                    anchorTime = pieces[a].seconds;
                }
            }
        grid.steadyShare = total > 0.0 ? steady / total : 0.0;
        if (grid.steadyShare >= p.steadyAboveShare && median > 0.0)
        {
            // The constant grid again, from the steady pieces: every beat in phase with the longest one (within
            // 20 ms) counts, the slipped ones don't, so the tempo comes out exact.
            // Fitted again a few times, so the line grows from that piece over the whole track.
            Fit line{anchorTime, 60.0 / median};
            std::vector<double> keptK, keptT;
            for (int pass = 0; pass < 5; ++pass)
            {
                std::vector<double> passK, passT;
                for (const double t : times)
                {
                    const double kk = std::round((t - line.t0) / line.period);
                    if (std::abs(t - (line.t0 + kk * line.period)) < 0.020)
                    {
                        passK.push_back(kk);
                        passT.push_back(t);
                    }
                }
                if (passK.size() < 8)
                    break;
                const Fit refit = fitLine(passK, passT);
                if (refit.period <= 0.0)
                    break;
                line = refit;
                keptK.swap(passK);
                keptT.swap(passT);
            }
            if (keptK.size() >= 8)
            {
                constK.swap(keptK);
                constT.swap(keptT);
                fit = line;
                index = constK; // the beat numbers now follow this grid
            }
        }
    }

    // Which grid keeps more of the track's beats (all of them, outliers too) within 25 ms of a grid line.
    const auto hits = [&](const std::vector<double>& gridTimes)
    {
        int n = 0;
        for (const double t : times)
        {
            const auto it = std::lower_bound(gridTimes.begin(), gridTimes.end(), t);
            double d = 1e9;
            if (it != gridTimes.end())
                d = *it - t;
            if (it != gridTimes.begin())
                d = std::min(d, t - *(it - 1));
            n += d <= 0.025 ? 1 : 0;
        }
        return static_cast<double>(n) / static_cast<double>(times.size());
    };
    std::vector<double> constantTimes, followTimes;
    for (double kk = index.front() - 1.0; kk <= index.back() + 1.0; kk += 1.0)
        constantTimes.push_back(fit.t0 + kk * fit.period);
    const double lastBeat = localK.back() - localK.front();
    for (std::size_t a = 0; a < anchors.size(); ++a)
    {
        const double until = a + 1 < anchors.size() ? anchors[a + 1].beat : lastBeat + 1.0;
        for (double b = anchors[a].beat; b < until; b += 1.0)
            followTimes.push_back(anchors[a].seconds + (b - anchors[a].beat) * 60.0 / anchors[a].bpm);
    }
    grid.constantHitRate = hits(constantTimes);
    grid.followHitRate = hits(followTimes);
    grid.variableTempo = grid.steadyShare < p.steadyAboveShare && grid.constantHitRate < p.variableBelowHits &&
                         grid.followHitRate - grid.constantHitRate >= p.variableAboveGain;
    if (grid.variableTempo)
    {
        index.swap(localK);
        times.swap(localT);
        fit = fitLine(index, times);
        grid.anchors = std::move(anchors);
    }
    else
    {
        index.swap(constK);
        times.swap(constT);
    }
    if (fit.period <= 0.0)
        return grid;
    grid.valid = true;
    // Variable tempo: the average, beats over time from the first to the last.
    grid.bpm = grid.variableTempo && times.back() > times.front()
                   ? 60.0 * (index.back() - index.front()) / (times.back() - times.front())
                   : 60.0 / fit.period;
    grid.beatsUsed = static_cast<int>(index.size());
    const double firstK = index.front();
    grid.firstBeatSeconds = grid.variableTempo ? times.front() : fit.t0 + firstK * fit.period;

    // 4. Drift: local tempo over 16-beat windows against the average.
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

    // 5. Downbeat: the phase of 4 with the strongest onsets on its beats (low band counts double).
    double phaseScore[4] = {};
    int phaseCount[4] = {};
    for (std::size_t i = 0; i < index.size(); ++i)
    {
        const double frame = (times[i] - env.frameOffsetSeconds) * fr;
        const int phase = static_cast<int>(std::lround(index[i] - firstK)) % 4;
        phaseScore[phase] += strengthAt(o, frame) + 2.0 * strengthAt(env.lowFlux, frame);
        ++phaseCount[phase];
    }
    int downbeatPhase = 0;
    for (int ph = 1; ph < 4; ++ph)
        if (phaseCount[ph] > 0 &&
            phaseScore[ph] / phaseCount[ph] > phaseScore[downbeatPhase] / std::max(1, phaseCount[downbeatPhase]))
            downbeatPhase = ph;
    grid.firstBeatInBar = (4 - downbeatPhase) % 4 + 1;
    return grid;
}
} // namespace

BeatGrid trackBeats(const float* mono, int numSamples, double sampleRate, const BeatTrackerParams& p)
{
    BeatGrid grid;
    const auto env = onsetEnvelope(mono, numSamples, sampleRate);
    const auto& o = env.flux;
    const double fr = env.frameRate;
    if (o.size() < static_cast<std::size_t>(fr * 8.0)) // under ~8 s: not enough to be sure
        return grid;

    // 1. Tempo candidates: comb filter over the autocorrelation (the beat lag and its multiples) with a prior for
    //    the preferred range; its strongest peaks, each brought into the preferred range by the octave rules.
    const int maxLag = static_cast<int>(std::ceil(4.0 * fr * 60.0 / p.minBpm)) + 2;
    const auto r = autocorrelation(o, maxLag);
    const auto comb = [&](double bpm)
    {
        const double lag = fr * 60.0 / bpm;
        return at(r, lag) + 0.5 * at(r, 2 * lag) + 0.33 * at(r, 3 * lag) + 0.25 * at(r, 4 * lag);
    };
    // For choosing the peak, half the lag counts too: eighth notes fall halfway between the beats of the right
    // tempo, and at 2/3 of it (where 1.5 beats also line up) nothing does. Not for the octave choice below, where
    // it would favour half the tempo.
    const auto combWithHalf = [&](double bpm) { return comb(bpm) + 0.3 * at(r, 0.5 * fr * 60.0 / bpm); };
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
    std::vector<double> bpms, scores;
    double meanScore = 0.0;
    for (double bpm = p.minBpm; bpm <= p.maxBpm; bpm += 0.02)
    {
        bpms.push_back(bpm);
        scores.push_back(combWithHalf(bpm));
        meanScore += scores.back();
    }
    meanScore /= static_cast<double>(std::max<std::size_t>(1, scores.size()));
    std::vector<std::pair<double, double>> peaks; // weighted score, bpm
    for (std::size_t i = 1; i + 1 < scores.size(); ++i)
        if (scores[i] >= scores[i - 1] && scores[i] > scores[i + 1])
            peaks.push_back({scores[i] * prior(bpms[i]), bpms[i]});
    if (peaks.empty())
        return grid;
    std::sort(peaks.begin(), peaks.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const double rawBest = combWithHalf(peaks.front().second);
    if (rawBest - meanScore < 0.05) // no periodicity to speak of
        return grid;

    // Octaves: the faster of two tempos that both fit the preferred range (a pulse at 2x explains the slower one
    // too); a tempo outside it halves / doubles into it.
    const auto octave = [&](double bpm)
    {
        const double own = comb(bpm);
        if (const auto [dbl, ds] = peakNear(bpm * 2.0, 0.01); dbl <= p.preferMax && dbl >= p.preferMin && ds >= 0.9 * own)
            return dbl;
        if (bpm > p.preferMax)
            if (const auto [half, hs] = peakNear(bpm / 2.0, 0.01); half >= p.preferMin && hs >= 0.5 * own)
                return half;
        if (bpm < p.preferMin)
            if (const auto [dbl, ds] = peakNear(bpm * 2.0, 0.01); dbl <= p.preferMax && ds >= 0.5 * own)
                return dbl;
        return bpm;
    };
    // (Choosing among the comb's other peaks by how well their beats sit on the onsets was tried on real tracks
    // and made things worse: a 2/3 tempo can fit the onsets of a shuffle better. So was doubling slow tempos when
    // the music hits as hard halfway between the beats: it doesn't tell a 90 BPM ballad from a 180 BPM song.)
    const double bestBpm = octave(peaks.front().second);
    const Chain chain = beatChain(o, fr * 60.0 / bestBpm);
    BeatGrid steady = fitGrid(env, bestBpm, chain, p);
    // Not steady and the constant grid misses most beats: follow the beats again with a chain that holds less
    // tightly to one period (a band drifting a few % fights a tight one), and keep that if its grid fits better.
    if (steady.valid && steady.steadyShare < p.steadyAboveShare && steady.constantHitRate < p.variableBelowHits)
    {
        BeatGrid loose = fitGrid(env, bestBpm, beatChain(o, fr * 60.0 / bestBpm, p.looseTightness), p);
        if (loose.valid && loose.variableTempo && loose.followHitRate > steady.followHitRate)
            return loose;
    }
    return steady;
}

std::string beatgridJson(const BeatGrid& grid, double trackSampleRate)
{
    if (!grid.valid)
        return "[]";
    const auto anchor = [&](int beat, double seconds, double bpm)
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "{\"beat\":%d,\"sample\":%lld,\"bpm\":%.4f,\"bar_beat\":%d}", beat,
                      static_cast<long long>(std::llround(seconds * trackSampleRate)), bpm,
                      (grid.firstBeatInBar - 1 + beat) % 4 + 1);
        return std::string(buf);
    };
    if (grid.anchors.empty())
        return "[" + anchor(0, grid.firstBeatSeconds, grid.bpm) + "]";
    std::string out = "[";
    for (const auto& a : grid.anchors)
        out += (out.size() > 1 ? "," : "") + anchor(a.beat, a.seconds, a.bpm);
    return out + "]";
}

} // namespace devildj::analysis
