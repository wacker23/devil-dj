#pragma once

#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace devildj::analysis
{

// A plain radix-2 FFT for the analyzers (P2-13 onsets, P2-15 chroma): offline, one size per instance, the
// twiddles precomputed. Not tuned for speed beyond that; analysis runs on background threads.
class Fft
{
public:
    explicit Fft(int size)
        : n(size)
        , twiddle(static_cast<std::size_t>(size / 2))
        , bitReverse(static_cast<std::size_t>(size))
    {
        int bits = 0;
        while ((1 << bits) < n)
            ++bits;
        for (int i = 0; i < n / 2; ++i)
            twiddle[static_cast<std::size_t>(i)] = std::polar(1.0, -2.0 * std::numbers::pi * i / n);
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                r |= ((i >> b) & 1) << (bits - 1 - b);
            bitReverse[static_cast<std::size_t>(i)] = r;
        }
    }

    int size() const noexcept { return n; }

    // In place, forward (e^-i).
    void forward(std::vector<std::complex<double>>& x) const
    {
        for (int i = 0; i < n; ++i)
            if (const int r = bitReverse[static_cast<std::size_t>(i)]; r > i)
                std::swap(x[static_cast<std::size_t>(i)], x[static_cast<std::size_t>(r)]);
        for (int len = 2; len <= n; len <<= 1)
        {
            const int step = n / len;
            for (int start = 0; start < n; start += len)
                for (int k = 0; k < len / 2; ++k)
                {
                    const auto w = twiddle[static_cast<std::size_t>(k * step)];
                    auto& a = x[static_cast<std::size_t>(start + k)];
                    auto& b = x[static_cast<std::size_t>(start + k + len / 2)];
                    const auto t = w * b;
                    b = a - t;
                    a += t;
                }
        }
    }

    // Magnitudes of bins 0..n/2 of a real frame (already windowed), into `out`.
    void magnitudes(const float* frame, std::vector<double>& out, std::vector<std::complex<double>>& work) const
    {
        work.assign(static_cast<std::size_t>(n), {});
        for (int i = 0; i < n; ++i)
            work[static_cast<std::size_t>(i)] = frame[i];
        forward(work);
        out.resize(static_cast<std::size_t>(n / 2 + 1));
        for (int k = 0; k <= n / 2; ++k)
            out[static_cast<std::size_t>(k)] = std::abs(work[static_cast<std::size_t>(k)]);
    }

private:
    int n;
    std::vector<std::complex<double>> twiddle;
    std::vector<int> bitReverse;
};

} // namespace devildj::analysis
