#include "DriftBuffer.h"

#include <algorithm>
#include <cmath>

namespace devildj::dsp
{

namespace
{
constexpr double kMaxCorrection = 0.005; // +-0.5 %: far beyond any clock drift, far below an audible wobble
constexpr double kGain = 0.01;           // correction per unit of relative fill error
constexpr double kSmoothingSeconds = 2.0;
} // namespace

DriftBuffer::DriftBuffer(int capacityFrames)
{
    std::int64_t size = 1;
    while (size < std::max(capacityFrames, 1024))
        size <<= 1;
    data.assign(static_cast<std::size_t>(size) * 2, 0.0f);
    mask = size - 1;
}

void DriftBuffer::setWriter(double sampleRate, int blockSize) noexcept
{
    if (sampleRate > 0.0)
        writerRate.store(sampleRate, std::memory_order_relaxed);
    if (blockSize > 0)
        writerBlock.store(blockSize, std::memory_order_relaxed);
}

void DriftBuffer::setReader(double sampleRate, int blockSize) noexcept
{
    if (sampleRate > 0.0)
        readerRate.store(sampleRate, std::memory_order_relaxed);
    if (blockSize > 0)
        readerBlock.store(blockSize, std::memory_order_relaxed);
}

double DriftBuffer::targetFrames() const noexcept
{
    return 1.5 * (writerBlock.load(std::memory_order_relaxed) + readerBlock.load(std::memory_order_relaxed)) + 128.0;
}

void DriftBuffer::restart() noexcept
{
    restartRequested.store(true, std::memory_order_release);
}

void DriftBuffer::write(const float* left, const float* right, int numFrames) noexcept
{
    if (numFrames <= 0)
        return;
    const auto w = written.load(std::memory_order_relaxed);
    const auto capacity = mask + 1;
    if (w + numFrames - readFloor.load(std::memory_order_acquire) > capacity)
    {
        overrunCount.fetch_add(1, std::memory_order_relaxed); // the reader isn't keeping up (or isn't running)
        return;
    }
    for (int i = 0; i < numFrames; ++i)
    {
        const auto at = static_cast<std::size_t>((w + i) & mask) * 2;
        data[at] = left != nullptr ? left[i] : 0.0f;
        data[at + 1] = right != nullptr ? right[i] : (left != nullptr ? left[i] : 0.0f);
    }
    written.store(w + numFrames, std::memory_order_release);
}

float DriftBuffer::sample(int channel, std::int64_t frame) const noexcept
{
    return data[static_cast<std::size_t>(frame & mask) * 2 + static_cast<std::size_t>(channel)];
}

void DriftBuffer::read(float* left, float* right, int numFrames) noexcept
{
    if (numFrames <= 0)
        return;
    const auto silence = [&](int from)
    {
        for (int i = from; i < numFrames; ++i)
        {
            if (left != nullptr)
                left[i] = 0.0f;
            if (right != nullptr)
                right[i] = 0.0f;
        }
    };
    const auto w = written.load(std::memory_order_acquire);
    if (restartRequested.exchange(false, std::memory_order_acq_rel))
    {
        priming.store(true, std::memory_order_relaxed);
        resumeFrom = w; // only what comes after the restart
    }

    const double target = targetFrames();
    if (priming.load(std::memory_order_relaxed))
    {
        // Wait for the target fill of new audio (after a restart or running dry); then start that far behind the
        // writer (older audio is skipped). While waiting the writer may go on filling.
        if (static_cast<double>(w - resumeFrom) < target + 4.0)
        {
            // Nothing before resumeFrom will be read: the writer may reuse it.
            readFloor.store(std::max<std::int64_t>(0, resumeFrom - 4), std::memory_order_release);
            silence(0);
            return;
        }
        readFloor.store(std::max<std::int64_t>(0, w - static_cast<std::int64_t>(target) - 4), std::memory_order_release);
        position = static_cast<double>(w) - target;
        priming.store(false, std::memory_order_relaxed);
        smoothedError = 0.0;
    }

    // Steer the ratio by the fill (smoothed over ~2 s of reader blocks).
    const double fill = static_cast<double>(w) - position;
    lastFill.store(fill, std::memory_order_relaxed);
    const double rateIn = writerRate.load(std::memory_order_relaxed);
    const double rateOut = readerRate.load(std::memory_order_relaxed);
    const double blockSeconds = numFrames / rateOut;
    smoothedError += std::min(1.0, blockSeconds / kSmoothingSeconds) * ((fill - target) / target - smoothedError);
    const double correction = std::clamp(kGain * smoothedError, -kMaxCorrection, kMaxCorrection);
    correctionShown.store(1.0 + correction, std::memory_order_relaxed);
    const double step = rateIn / rateOut * (1.0 + correction);

    for (int i = 0; i < numFrames; ++i)
    {
        const auto base = static_cast<std::int64_t>(std::floor(position));
        if (base + 2 >= w) // ran dry: silence and prime again
        {
            underrunCount.fetch_add(1, std::memory_order_relaxed);
            priming.store(true, std::memory_order_relaxed);
            resumeFrom = w;
            silence(i);
            break;
        }
        const double t = position - static_cast<double>(base);
        for (int ch = 0; ch < 2; ++ch)
        {
            float* out = ch == 0 ? left : right;
            if (out == nullptr)
                continue;
            const double y0 = sample(ch, base - 1), y1 = sample(ch, base), y2 = sample(ch, base + 1),
                         y3 = sample(ch, base + 2);
            // Cubic Hermite (Catmull-Rom).
            const double c1 = 0.5 * (y2 - y0);
            const double c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
            const double c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
            out[i] = static_cast<float>(((c3 * t + c2) * t + c1) * t + y1);
        }
        position += step;
    }
    readFloor.store(std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(position)) - 1),
                    std::memory_order_release);
}

} // namespace devildj::dsp
