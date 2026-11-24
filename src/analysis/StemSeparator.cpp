#include "analysis/StemSeparator.h"

#include "analysis/Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace devildj::analysis
{

namespace demucs
{

namespace
{
constexpr int kPad = kHop / 2 * 3;                          // 1536: demucs's own padding, a quarter n_fft and a half
constexpr int kPadded = kFrames * kHop + 2 * kPad;           // 347136: the signal the STFT sees
constexpr int kStftFrames = kFrames + 4;                     // 340: with the two frames each side demucs drops

int reflect(int i, int n)
{
    if (n <= 1)
        return 0;
    while (i < 0 || i >= n)
        i = i < 0 ? -i : 2 * (n - 1) - i;
    return i;
}

const std::vector<double>& window()
{
    static const std::vector<double> w = []
    {
        std::vector<double> v(kFft);
        for (int i = 0; i < kFft; ++i)
            v[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kFft); // periodic Hann
        return v;
    }();
    return w;
}

const Fft& fft()
{
    static const Fft f(kFft);
    return f;
}
} // namespace

std::vector<float> spectrogram(const float* left, const float* right)
{
    // demucs: the segment reflect-padded by kPad (and up to whole hops on the right), then torch.stft with
    // center=True (another n_fft / 2, reflect), normalized (/ sqrt(n_fft)); the last bin and 2 frames each side
    // dropped.
    std::vector<float> out(static_cast<std::size_t>(4) * kBins * kFrames);
    const auto& w = window();
    const double norm = std::sqrt(static_cast<double>(kFft));
    std::vector<std::complex<double>> work(kFft);
    const float* channels[2] = {left, right};
    for (int c = 0; c < 2; ++c)
    {
        const float* x = channels[c];
        // Sample `p` of the STFT's own (centre-padded) signal, in the segment.
        const auto at = [x](int centred)
        {
            const int padded = reflect(centred - kFft / 2, kPadded); // torch.stft's centre padding
            return x[reflect(padded - kPad, kSegment)];              // demucs's padding
        };
        for (int t = 0; t < kFrames; ++t)
        {
            const int frame = t + 2;
            for (int n = 0; n < kFft; ++n)
                work[static_cast<std::size_t>(n)] = at(frame * kHop + n) * w[static_cast<std::size_t>(n)];
            fft().forward(work);
            for (int k = 0; k < kBins; ++k)
            {
                const auto z = work[static_cast<std::size_t>(k)] / norm;
                out[(static_cast<std::size_t>(2 * c) * kBins + static_cast<std::size_t>(k)) * kFrames + static_cast<std::size_t>(t)] =
                    static_cast<float>(z.real());
                out[(static_cast<std::size_t>(2 * c + 1) * kBins + static_cast<std::size_t>(k)) * kFrames + static_cast<std::size_t>(t)] =
                    static_cast<float>(z.imag());
            }
        }
    }
    return out;
}

void inverse(const float* spec, int length, float* left, float* right)
{
    // demucs: a zero bin on top, two zero frames each side, then torch.istft (normalized, center=True) to
    // kPadded samples, of which [kPad, kPad + length) are the segment.
    const auto& w = window();
    const double scale = std::sqrt(static_cast<double>(kFft)); // undoes the forward normalisation
    const int olaLength = kFft + kHop * (kStftFrames - 1);
    std::vector<double> ola(static_cast<std::size_t>(olaLength)), env(static_cast<std::size_t>(olaLength));
    std::vector<std::complex<double>> work(kFft);
    float* outs[2] = {left, right};
    for (int c = 0; c < 2; ++c)
    {
        std::fill(ola.begin(), ola.end(), 0.0);
        std::fill(env.begin(), env.end(), 0.0);
        const float* re = spec + static_cast<std::size_t>(2 * c) * kBins * kFrames;
        const float* im = spec + static_cast<std::size_t>(2 * c + 1) * kBins * kFrames;
        for (int frame = 0; frame < kStftFrames; ++frame)
        {
            const int t = frame - 2;
            // The inverse real FFT through a forward one: ifft(X) = conj(fft(conj(X))) / N, X Hermitian.
            for (int k = 0; k <= kFft / 2; ++k)
            {
                std::complex<double> z{};
                if (t >= 0 && t < kFrames && k < kBins)
                    z = {re[static_cast<std::size_t>(k) * kFrames + static_cast<std::size_t>(t)],
                         im[static_cast<std::size_t>(k) * kFrames + static_cast<std::size_t>(t)]};
                if (k == 0 || k == kFft / 2)
                    z = {z.real(), 0.0}; // irfft ignores the imaginary part at DC and Nyquist
                work[static_cast<std::size_t>(k)] = std::conj(z);
                if (k > 0 && k < kFft / 2)
                    work[static_cast<std::size_t>(kFft - k)] = z; // conj of the mirrored conj
            }
            fft().forward(work);
            for (int n = 0; n < kFft; ++n)
            {
                const double y = work[static_cast<std::size_t>(n)].real() / kFft * scale; // conj: same real part
                const auto i = static_cast<std::size_t>(frame * kHop + n);
                ola[i] += y * w[static_cast<std::size_t>(n)];
                env[i] += w[static_cast<std::size_t>(n)] * w[static_cast<std::size_t>(n)];
            }
        }
        for (int i = 0; i < length; ++i)
        {
            const auto j = static_cast<std::size_t>(i + kPad + kFft / 2); // istft's centre trim, then demucs's pad
            outs[c][i] = env[j] > 1e-11 ? static_cast<float>(ola[j] / env[j]) : 0.0f;
        }
    }
}

} // namespace demucs

util::Result<std::unique_ptr<StemSeparator>> StemSeparator::open(const std::filesystem::path& model, InferenceOptions options)
{
    using R = util::Result<std::unique_ptr<StemSeparator>>;
    // On the CPU: with ONNX Runtime 1.24's DirectML provider this network gives wrong results (off by 1e5 against
    // PyTorch, with any graph optimisation level - tested), while the CPU matches it to 3e-5.
    options.preferGpu = false;
    auto s = InferenceSession::open(model, options);
    if (!s)
        return R::fail(s.error());
    std::unique_ptr<StemSeparator> sep(new StemSeparator());
    sep->session = std::move(s.value());
    return R::ok(std::move(sep));
}

util::Result<std::vector<float>> StemSeparator::separateSegment(const float* left, const float* right)
{
    using namespace demucs;
    using R = util::Result<std::vector<float>>;
    Tensor mag{{1, 4, kBins, kFrames}, spectrogram(left, right)};
    Tensor mix{{1, 2, kSegment}, std::vector<float>(static_cast<std::size_t>(2) * kSegment)};
    std::copy(left, left + kSegment, mix.data.begin());
    std::copy(right, right + kSegment, mix.data.begin() + kSegment);
    auto out = session->run({mag, mix});
    if (!out)
        return R::fail(out.error());
    if (out.value().size() != 2)
        return R::fail("the stem model should give 2 outputs");
    const auto& spec = out.value()[0].data; // [1][S][4][F][T]
    const auto& wave = out.value()[1].data; // [1][S][2][L]
    if (spec.size() != static_cast<std::size_t>(kSources) * 4 * kBins * kFrames || wave.size() != static_cast<std::size_t>(kSources) * 2 * kSegment)
        return R::fail("the stem model's outputs aren't the expected size");
    std::vector<float> result(static_cast<std::size_t>(kSources) * 2 * kSegment);
    for (int s = 0; s < kSources; ++s)
    {
        float* l = result.data() + static_cast<std::size_t>(s * 2) * kSegment;
        float* r = l + kSegment;
        inverse(spec.data() + static_cast<std::size_t>(s) * 4 * kBins * kFrames, kSegment, l, r);
        const float* wl = wave.data() + static_cast<std::size_t>(s * 2) * kSegment;
        const float* wr = wl + kSegment;
        for (int i = 0; i < kSegment; ++i)
        {
            l[i] += wl[i];
            r[i] += wr[i];
        }
    }
    return R::ok(std::move(result));
}

util::Result<bool> StemSeparator::separate(const float* left, const float* right, std::int64_t numSamples, Quality quality,
                                           const Sink& sink, const std::function<bool(double)>& progress)
{
    using namespace demucs;
    using R = util::Result<bool>;
    if (numSamples <= 0)
        return R::ok(true);
    // The track normalised by its mono mean and (sample) deviation, as demucs's separate does; undone on the way out.
    double sum = 0.0;
    for (std::int64_t i = 0; i < numSamples; ++i)
        sum += 0.5 * (static_cast<double>(left[i]) + right[i]);
    const double mean = sum / static_cast<double>(numSamples);
    double var = 0.0;
    for (std::int64_t i = 0; i < numSamples; ++i)
    {
        const double d = 0.5 * (static_cast<double>(left[i]) + right[i]) - mean;
        var += d * d;
    }
    const double stdev = std::sqrt(var / static_cast<double>(std::max<std::int64_t>(1, numSamples - 1)));
    const double inv = stdev > 0.0 ? 1.0 / stdev : 1.0;

    // demucs.apply: a quarter overlap, triangular weights (1 .. peak .. 1, scaled to a peak of 1).
    const int stride = static_cast<int>(0.75 * kSegment);
    std::vector<float> weight(static_cast<std::size_t>(kSegment));
    for (int i = 0; i < kSegment; ++i)
        weight[static_cast<std::size_t>(i)] = static_cast<float>(i < kSegment / 2 ? i + 1 : kSegment - i) / static_cast<float>(kSegment / 2);
    // Passes per segment: unshifted, and for quality two shifts of the input (demucs's shift trick).
    std::vector<int> shifts{0};
    if (quality == Quality::Quality)
        shifts = {0, static_cast<int>(0.5 * kSampleRate / 4), static_cast<int>(0.5 * kSampleRate * 3 / 4)};

    std::vector<std::int64_t> offsets;
    for (std::int64_t o = 0; o < numSamples; o += stride)
        offsets.push_back(o);
    // The rolling output: [base, base + kSegment) of each of the 8 channels, and the weights summed there.
    std::array<std::vector<double>, 8> acc;
    for (auto& a : acc)
        a.assign(static_cast<std::size_t>(kSegment), 0.0);
    std::vector<double> wsum(static_cast<std::size_t>(kSegment), 0.0);
    std::int64_t base = 0;
    std::vector<float> inL(static_cast<std::size_t>(kSegment)), inR(static_cast<std::size_t>(kSegment));
    std::array<std::vector<float>, 8> emitted;
    const auto emit = [&](int count)
    {
        // The first `count` samples of the window are final: averaged, de-normalised, given to the sink.
        const float* ptrs[8];
        for (int ch = 0; ch < 8; ++ch)
        {
            auto& e = emitted[static_cast<std::size_t>(ch)];
            e.resize(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i)
            {
                const double ws = wsum[static_cast<std::size_t>(i)];
                e[static_cast<std::size_t>(i)] = static_cast<float>((ws > 0.0 ? acc[static_cast<std::size_t>(ch)][static_cast<std::size_t>(i)] / ws : 0.0) * stdev + mean);
            }
            ptrs[ch] = e.data();
        }
        // "other" is what is left: the mix less drums, bass and vocals, so the four stems always add up to the track
        // (the model's own four don't, by 20-30 dB on real music; with them all up a deck must sound as the track).
        for (int c = 0; c < 2; ++c)
        {
            const float* mixCh = c == 0 ? left : right;
            auto& other = emitted[static_cast<std::size_t>(2 * 2 + c)];
            for (int i = 0; i < count; ++i)
                other[static_cast<std::size_t>(i)] = mixCh[base + i] - emitted[static_cast<std::size_t>(0 * 2 + c)][static_cast<std::size_t>(i)] -
                                                     emitted[static_cast<std::size_t>(1 * 2 + c)][static_cast<std::size_t>(i)] -
                                                     emitted[static_cast<std::size_t>(3 * 2 + c)][static_cast<std::size_t>(i)];
        }
        sink(ptrs, count);
        for (auto& a : acc)
        {
            std::move(a.begin() + count, a.end(), a.begin());
            std::fill(a.end() - count, a.end(), 0.0);
        }
        std::move(wsum.begin() + count, wsum.end(), wsum.begin());
        std::fill(wsum.end() - count, wsum.end(), 0.0);
        base += count;
    };

    const double totalPasses = static_cast<double>(offsets.size() * shifts.size());
    double done = 0.0;
    for (std::size_t ci = 0; ci < offsets.size(); ++ci)
    {
        const std::int64_t o = offsets[ci];
        const int length = static_cast<int>(std::min<std::int64_t>(kSegment, numSamples - o));
        const int delta = kSegment - length;
        std::vector<double> chunk(static_cast<std::size_t>(8) * length, 0.0);
        for (const int shift : shifts)
        {
            // The segment around the chunk (TensorChunk.padded: centred, the track's own audio, zeros past its
            // ends), `shift` samples earlier for a shifted pass.
            const std::int64_t start = o - delta / 2 - shift;
            for (int i = 0; i < kSegment; ++i)
            {
                const std::int64_t idx = start + i;
                const bool in = idx >= 0 && idx < numSamples;
                inL[static_cast<std::size_t>(i)] = in ? static_cast<float>((left[idx] - mean) * inv) : 0.0f;
                inR[static_cast<std::size_t>(i)] = in ? static_cast<float>((right[idx] - mean) * inv) : 0.0f;
            }
            auto out = separateSegment(inL.data(), inR.data());
            if (!out)
                return R::fail(out.error());
            // Chunk sample j is segment sample delta / 2 + shift + j (center_trim, then the shift undone).
            const int from = delta / 2 + shift;
            for (int ch = 0; ch < 8; ++ch)
                for (int j = 0; j < length; ++j)
                {
                    const int k = from + j;
                    if (k < kSegment)
                        chunk[static_cast<std::size_t>(ch) * length + static_cast<std::size_t>(j)] +=
                            out.value()[static_cast<std::size_t>(ch) * kSegment + static_cast<std::size_t>(k)] / static_cast<double>(shifts.size());
                }
            done += 1.0;
            if (progress && !progress(done / totalPasses))
                return R::fail("cancelled");
        }
        const auto rel = static_cast<std::size_t>(o - base);
        for (int ch = 0; ch < 8; ++ch)
            for (int j = 0; j < length; ++j)
                acc[static_cast<std::size_t>(ch)][rel + static_cast<std::size_t>(j)] +=
                    weight[static_cast<std::size_t>(j)] * chunk[static_cast<std::size_t>(ch) * length + static_cast<std::size_t>(j)];
        for (int j = 0; j < length; ++j)
            wsum[rel + static_cast<std::size_t>(j)] += weight[static_cast<std::size_t>(j)];
        // Everything before the next segment's start is final.
        const std::int64_t finalUpTo = ci + 1 < offsets.size() ? offsets[ci + 1] : numSamples;
        emit(static_cast<int>(finalUpTo - base));
    }
    return R::ok(true);
}

} // namespace devildj::analysis
