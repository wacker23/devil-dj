#include "analysis/BeatThis.h"

#include "analysis/Fft.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

namespace devildj::analysis
{

namespace beatthis
{

namespace
{
// Slaney's mel scale: linear below 1 kHz (200/3 Hz per mel), logarithmic above.
constexpr double kFSp = 200.0 / 3.0, kMinLogHz = 1000.0, kMinLogMel = kMinLogHz / kFSp;
const double kLogStep = std::log(6.4) / 27.0;

double hzToMel(double hz)
{
    return hz < kMinLogHz ? hz / kFSp : kMinLogMel + std::log(hz / kMinLogHz) / kLogStep;
}

double melToHz(double mel)
{
    return mel < kMinLogMel ? mel * kFSp : kMinLogHz * std::exp(kLogStep * (mel - kMinLogMel));
}

int reflect(int i, int n)
{
    // Reflection without repeating the edge sample (... x2 x1 | x0 x1 x2 ... xn-1 | xn-2 ...).
    if (n <= 1)
        return 0;
    while (i < 0 || i >= n)
        i = i < 0 ? -i : 2 * (n - 1) - i;
    return i;
}
} // namespace

std::vector<float> melFilterbank()
{
    // Triangles between mel-spaced points (no area normalisation), over the FFT bins' frequencies 0 .. sr/2.
    constexpr double fMin = 30.0, fMax = 11000.0;
    const double mMin = hzToMel(fMin), mMax = hzToMel(fMax);
    std::vector<double> fPts(kMels + 2);
    for (int i = 0; i < kMels + 2; ++i)
        fPts[static_cast<std::size_t>(i)] = melToHz(mMin + (mMax - mMin) * i / (kMels + 1));
    std::vector<float> fb(static_cast<std::size_t>(kBins * kMels), 0.0f);
    for (int k = 0; k < kBins; ++k)
    {
        const double f = (kSampleRate / 2.0) * k / (kBins - 1);
        for (int m = 0; m < kMels; ++m)
        {
            const double lo = fPts[static_cast<std::size_t>(m)], mid = fPts[static_cast<std::size_t>(m + 1)],
                         hi = fPts[static_cast<std::size_t>(m + 2)];
            const double down = (f - lo) / (mid - lo), up = (hi - f) / (hi - mid);
            fb[static_cast<std::size_t>(k * kMels + m)] = static_cast<float>(std::max(0.0, std::min(down, up)));
        }
    }
    return fb;
}

int frameCount(int numSamples)
{
    return numSamples <= 0 ? 0 : 1 + numSamples / kHop;
}

std::vector<float> spectrogram(const float* mono, int numSamples)
{
    const int frames = frameCount(numSamples);
    std::vector<float> out(static_cast<std::size_t>(frames) * kMels);
    if (frames == 0)
        return out;
    const auto fb = melFilterbank();
    std::vector<double> window(kFft);
    for (int i = 0; i < kFft; ++i)
        window[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kFft); // periodic
    // torchaudio's normalized="frame_length": the STFT divided by sqrt(n_fft).
    const double norm = std::sqrt(static_cast<double>(kFft));
    const Fft fft(kFft);
    std::vector<std::complex<double>> work(kFft);
    std::vector<double> mag(kBins);
    for (int t = 0; t < frames; ++t)
    {
        // Centred: frame t covers samples t * hop - n_fft / 2 ..., reflect-padded at the ends.
        const int start = t * kHop - kFft / 2;
        for (int i = 0; i < kFft; ++i)
            work[static_cast<std::size_t>(i)] = mono[reflect(start + i, numSamples)] * window[static_cast<std::size_t>(i)];
        fft.forward(work);
        for (int k = 0; k < kBins; ++k)
            mag[static_cast<std::size_t>(k)] = std::abs(work[static_cast<std::size_t>(k)]) / norm;
        for (int m = 0; m < kMels; ++m)
        {
            double sum = 0.0;
            for (int k = 0; k < kBins; ++k)
                sum += fb[static_cast<std::size_t>(k * kMels + m)] * mag[static_cast<std::size_t>(k)];
            out[static_cast<std::size_t>(t) * kMels + static_cast<std::size_t>(m)] = static_cast<float>(std::log1p(1000.0 * sum));
        }
    }
    return out;
}

namespace
{
// Peak frames of one logit curve (as seconds): the maximum of its +-3 frames and above 0; neighbours merged.
std::vector<double> peakTimes(const std::vector<float>& logits)
{
    const int n = static_cast<int>(logits.size());
    std::vector<int> frames;
    for (int i = 0; i < n; ++i)
    {
        float m = logits[static_cast<std::size_t>(i)];
        for (int j = std::max(0, i - 3); j <= std::min(n - 1, i + 3); ++j)
            m = std::max(m, logits[static_cast<std::size_t>(j)]);
        if (logits[static_cast<std::size_t>(i)] == m && logits[static_cast<std::size_t>(i)] > 0.0f)
            frames.push_back(i);
    }
    std::vector<double> out;
    for (std::size_t i = 0; i < frames.size();)
    {
        std::size_t j = i;
        double sum = frames[i];
        while (j + 1 < frames.size() && frames[j + 1] - frames[j] <= 1)
            sum += frames[++j];
        out.push_back(sum / static_cast<double>(j - i + 1) / kFps);
        i = j + 1;
    }
    return out;
}
} // namespace

Peaks peaks(const std::vector<float>& beatLogits, const std::vector<float>& downbeatLogits)
{
    Peaks p;
    p.beats = peakTimes(beatLogits);
    for (const double d : peakTimes(downbeatLogits))
    {
        if (p.beats.empty())
            break;
        const auto nearest = *std::min_element(p.beats.begin(), p.beats.end(),
                                               [d](double a, double b) { return std::abs(a - d) < std::abs(b - d); });
        if (p.downbeats.empty() || p.downbeats.back() != nearest)
            p.downbeats.push_back(nearest);
    }
    std::sort(p.downbeats.begin(), p.downbeats.end());
    p.downbeats.erase(std::unique(p.downbeats.begin(), p.downbeats.end()), p.downbeats.end());
    return p;
}

BeatGrid gridFromBeats(const std::vector<double>& beats, const std::vector<double>& downbeats)
{
    BeatGrid g;
    if (beats.size() < 8)
        return g;
    // A first period from the median interval, then a least-squares line through (beat number, time), with the
    // beats more than 40 ms off it left out, twice.
    std::vector<double> intervals;
    for (std::size_t i = 1; i < beats.size(); ++i)
        intervals.push_back(beats[i] - beats[i - 1]);
    std::nth_element(intervals.begin(), intervals.begin() + static_cast<std::ptrdiff_t>(intervals.size() / 2), intervals.end());
    double period = intervals[intervals.size() / 2];
    double t0 = beats.front();
    if (period <= 0.0)
        return g;
    std::vector<std::pair<double, double>> used; // (beat number, time)
    for (int pass = 0; pass < 3; ++pass)
    {
        used.clear();
        if (pass == 0)
        {
            // The first numbering gap by gap (each gap rounded to whole periods: a missed beat is a gap of 2), so a
            // period a little off - the frames are 20 ms apart - doesn't drift over hundreds of beats.
            double k = 0.0;
            used.emplace_back(0.0, beats.front());
            for (std::size_t i = 1; i < beats.size(); ++i)
            {
                const double gap = std::round((beats[i] - beats[i - 1]) / period);
                if (gap < 1.0)
                    continue; // a stray between two beats
                k += gap;
                used.emplace_back(k, beats[i]);
            }
        }
        else
            for (const double b : beats)
            {
                const double k = std::round((b - t0) / period);
                if (std::abs(t0 + k * period - b) <= 0.04)
                    used.emplace_back(k, b);
            }
        if (used.size() < 8)
            return g;
        double sk = 0, st = 0, skk = 0, skt = 0;
        for (const auto& [k, t] : used)
        {
            sk += k;
            st += t;
            skk += k * k;
            skt += k * t;
        }
        const double n = static_cast<double>(used.size());
        const double den = n * skk - sk * sk;
        if (den <= 0.0)
            return g;
        period = (n * skt - sk * st) / den;
        t0 = (st - period * sk) / n;
    }
    // The grid starts at the first beat it fits.
    const double firstK = std::min_element(used.begin(), used.end())->first;
    g.firstBeatSeconds = t0 + firstK * period;
    g.bpm = 60.0 / period;
    int hits = 0;
    for (const double b : beats)
        hits += std::abs(t0 + std::round((b - t0) / period) * period - b) <= 0.025 ? 1 : 0;
    g.constantHitRate = static_cast<double>(hits) / static_cast<double>(beats.size());
    g.constantFraction = static_cast<double>(used.size()) / static_cast<double>(beats.size());
    g.beatsUsed = static_cast<int>(used.size());
    g.tempogramBpm = 60.0 / intervals[intervals.size() / 2];
    // The bar: the phase (of 4, from the first beat) where most downbeats fall.
    int votes[4] = {};
    for (const double d : downbeats)
    {
        const auto k = static_cast<long long>(std::llround((d - g.firstBeatSeconds) / period));
        ++votes[((k % 4) + 4) % 4];
    }
    const int phase = static_cast<int>(std::max_element(votes, votes + 4) - votes);
    g.firstBeatInBar = (4 - phase) % 4 + 1; // phase 0: the first beat is a downbeat
    g.valid = g.bpm >= 40.0 && g.bpm <= 250.0;
    return g;
}

} // namespace beatthis

util::Result<std::unique_ptr<BeatThisTracker>> BeatThisTracker::open(const std::filesystem::path& model, InferenceOptions options)
{
    auto s = InferenceSession::open(model, options);
    if (!s)
        return util::Result<std::unique_ptr<BeatThisTracker>>::fail(s.error());
    std::unique_ptr<BeatThisTracker> t(new BeatThisTracker());
    t->session = std::move(s.value());
    return util::Result<std::unique_ptr<BeatThisTracker>>::ok(std::move(t));
}

util::Result<std::pair<std::vector<float>, std::vector<float>>> BeatThisTracker::logits(const std::vector<float>& spect)
{
    using namespace beatthis;
    using R = util::Result<std::pair<std::vector<float>, std::vector<float>>>;
    const int frames = static_cast<int>(spect.size() / kMels);
    std::vector<float> beat(static_cast<std::size_t>(frames), -1000.0f), down(static_cast<std::size_t>(frames), -1000.0f);
    if (frames == 0)
        return R::ok({beat, down});
    // Chunk starts: from -border, a chunk less two borders apart; the last moved to end at the piece's end.
    std::vector<int> starts;
    for (int s = -kBorder; s < frames - kBorder; s += kChunk - 2 * kBorder)
        starts.push_back(s);
    if (frames > kChunk - 2 * kBorder)
        starts.back() = frames - (kChunk - kBorder);
    // Later chunks first, so the earlier one's frames are the ones kept where they overlap.
    for (auto it = starts.rbegin(); it != starts.rend(); ++it)
    {
        const int s = *it;
        const int from = std::max(s, 0), to = std::min(s + kChunk, frames);
        const int left = std::max(0, -s), right = std::max(0, std::min(kBorder, s + kChunk - frames));
        const int length = left + (to - from) + right;
        Tensor in{{1, length, kMels}, std::vector<float>(static_cast<std::size_t>(length) * kMels, 0.0f)};
        std::copy(spect.begin() + static_cast<std::ptrdiff_t>(from) * kMels, spect.begin() + static_cast<std::ptrdiff_t>(to) * kMels,
                  in.data.begin() + static_cast<std::ptrdiff_t>(left) * kMels);
        auto out = session->run({in});
        if (!out)
            return R::fail(out.error());
        if (out.value().size() != 2)
            return R::fail("the beat model should give 2 outputs");
        const auto& b = out.value()[0].data;
        const auto& d = out.value()[1].data;
        // Chunk frame j is piece frame (from - left) + j; its borders aren't kept.
        for (int j = kBorder; j < length - kBorder; ++j)
        {
            const int f = from - left + j;
            if (f < 0 || f >= frames || static_cast<std::size_t>(j) >= b.size())
                continue;
            beat[static_cast<std::size_t>(f)] = b[static_cast<std::size_t>(j)];
            down[static_cast<std::size_t>(f)] = d[static_cast<std::size_t>(j)];
        }
    }
    return R::ok({std::move(beat), std::move(down)});
}

util::Result<BeatThisTracker::Output> BeatThisTracker::track(const float* mono, int numSamples)
{
    const auto spect = beatthis::spectrogram(mono, numSamples);
    auto l = logits(spect);
    if (!l)
        return util::Result<Output>::fail(l.error());
    Output o;
    o.beatLogits = std::move(l.value().first);
    o.downbeatLogits = std::move(l.value().second);
    o.peaks = beatthis::peaks(o.beatLogits, o.downbeatLogits);
    o.grid = beatthis::gridFromBeats(o.peaks.beats, o.peaks.downbeats);
    return util::Result<Output>::ok(std::move(o));
}

const BeatGrid& chooseGrid(const BeatGrid& v1, const BeatGrid& v2)
{
    if (!v2.valid)
        return v1;
    if (!v1.valid)
        return v2;
    if (v1.variableTempo && v2.constantHitRate < v1.followHitRate + 0.1)
        return v1; // a live band: keep the grid that follows it
    return v2.constantHitRate > v1.constantHitRate ? v2 : v1;
}

} // namespace devildj::analysis
