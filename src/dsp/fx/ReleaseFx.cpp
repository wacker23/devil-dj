#include "dsp/fx/ReleaseFx.h"

#include "dsp/fx/BeatFx.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace devildj::dsp::fx
{

namespace
{
constexpr double kHistorySeconds = 16.0;
constexpr double kSpinRate = 3.0;
constexpr float kSilence = 1.0e-5f;
} // namespace

const char* ReleaseFx::kindName(Kind kind) noexcept
{
    switch (kind)
    {
        case Kind::EchoOut:
            return "Echo Out";
        case Kind::Brake:
            return "Brake";
        case Kind::SpinBack:
            return "Spin Back";
        case Kind::FilterSweep:
            return "Filter Sweep";
        case Kind::Count:
            break;
    }
    return "";
}

ReleaseFx::ReleaseFx() = default;
ReleaseFx::~ReleaseFx() = default;

void ReleaseFx::prepare(double rate, int maxBlockSize)
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    const auto block = static_cast<std::size_t>(std::max(1, maxBlockSize));
    if (echo == nullptr)
        echo = makeEffect(EffectType::Echo);
    echo->prepare(sampleRate, static_cast<int>(block));
    echo->setParam(0, 0.65f / 0.9f); // feedback 0.65
    echo->setParam(1, 0.7f);
    wetL.assign(block, 0.0f);
    wetR.assign(block, 0.0f);
    const auto history = static_cast<std::size_t>(kHistorySeconds * sampleRate) + 4;
    histL.assign(history, 0.0f);
    histR.assign(history, 0.0f);
    dry.prepare(sampleRate, 0.01);
    reset();
}

void ReleaseFx::reset() noexcept
{
    current = Stage::Idle;
    dry.snap(1.0);
    elapsed = readBack = 0.0;
    write = filled = 0;
    silentSamples = 0;
    hpIc1[0] = hpIc1[1] = hpIc2[0] = hpIc2[1] = 0.0;
    if (echo != nullptr)
        echo->reset();
}

void ReleaseFx::press(Kind kind, double b) noexcept
{
    if (current != Stage::Idle || kind == Kind::Count)
        return;
    currentKind = kind;
    beats = std::clamp(b, 1.0 / 16.0, 16.0);
    echo->reset();
    echo->setBeats(beats);
    elapsed = 0.0;
    hpIc1[0] = hpIc1[1] = hpIc2[0] = hpIc2[1] = 0.0;
    current = Stage::Held;
}

void ReleaseFx::release() noexcept
{
    if (current != Stage::Held)
        return;
    current = Stage::Released;
    elapsed = 0.0;
    readBack = 0.0;
    silentSamples = 0;
    dry.setTarget(0.0);
}

double ReleaseFx::lengthSamples(const BeatContext& beat) const noexcept
{
    const double bpm = std::clamp(beat.bpm > 0.0 ? beat.bpm : 120.0, 20.0, 300.0);
    return std::max(1.0, beats * 60.0 / bpm * sampleRate);
}

float ReleaseFx::hp(int c, float x, double hz) noexcept
{
    const double g = std::tan(std::numbers::pi * std::clamp(hz, 10.0, 0.45 * sampleRate) / sampleRate);
    constexpr double k = 1.4;
    const double a1 = 1.0 / (1.0 + g * (g + k));
    const double a2 = g * a1;
    const double a3 = g * a2;
    const double v3 = x - hpIc2[c];
    const double v1 = a1 * hpIc1[c] + a2 * v3;
    const double v2 = hpIc2[c] + a2 * hpIc1[c] + a3 * v3;
    hpIc1[c] = 2.0 * v1 - hpIc1[c];
    hpIc2[c] = 2.0 * v2 - hpIc2[c];
    return static_cast<float>(x - k * v1 - v2);
}

void ReleaseFx::process(float* left, float* right, int numSamples, const BeatContext& beat) noexcept
{
    const std::size_t size = histL.size();
    if (current == Stage::Idle) // untouched; the recent past is kept for a spin-back
    {
        for (int i = 0; i < numSamples; ++i)
        {
            histL[write] = left[i];
            histR[write] = right[i];
            write = write + 1 == size ? 0 : write + 1;
        }
        filled = std::min(filled + static_cast<std::size_t>(numSamples), size);
        return;
    }
    if (current == Stage::Finished)
    {
        std::fill(left, left + numSamples, 0.0f);
        std::fill(right, right + numSamples, 0.0f);
        return;
    }
    const double length = lengthSamples(beat);
    switch (currentKind)
    {
        case Kind::EchoOut:
        {
            for (int done = 0; done < numSamples;)
            {
                const int n = std::min(numSamples - done, static_cast<int>(wetL.size()));
                float* l = left + done;
                float* r = right + done;
                LinearRamp send = dry; // the echo hears the music only until the release cuts it
                for (int i = 0; i < n; ++i)
                {
                    const auto s = static_cast<float>(send.next());
                    wetL[static_cast<std::size_t>(i)] = l[i] * s;
                    wetR[static_cast<std::size_t>(i)] = r[i] * s;
                }
                echo->process(wetL.data(), wetR.data(), n, beat);
                float peak = 0.0f;
                for (int i = 0; i < n; ++i)
                {
                    const auto d = static_cast<float>(dry.next());
                    const float el = wetL[static_cast<std::size_t>(i)], er = wetR[static_cast<std::size_t>(i)];
                    l[i] = d * l[i] + 0.7f * el;
                    r[i] = d * r[i] + 0.7f * er;
                    peak = std::max({peak, std::abs(el), std::abs(er)});
                }
                if (current == Stage::Released)
                {
                    elapsed += n;
                    silentSamples = peak < kSilence ? silentSamples + n : 0;
                    if (silentSamples > static_cast<int>(0.05 * sampleRate) || elapsed > 8.0 * length)
                        current = Stage::Finished;
                }
                done += n;
            }
            break;
        }
        case Kind::Brake:
        case Kind::SpinBack:
        {
            for (int i = 0; i < numSamples; ++i)
            {
                // The history keeps the music as it goes on (the deck still plays under the cut).
                histL[write] = left[i];
                histR[write] = right[i];
                write = write + 1 == size ? 0 : write + 1;
                filled = std::min(filled + 1, size);
                if (current == Stage::Held)
                    continue; // the music passes
                // Released: read it ever further behind - slowing (brake) or backwards (spin-back) - to a stop.
                const double t = std::min(1.0, elapsed / length);
                const double speed = currentKind == Kind::Brake ? 1.0 - t : -kSpinRate * (1.0 - t) * (1.0 - t);
                readBack += 1.0 - speed; // how far behind the newest sample the read point is
                elapsed += 1.0;
                const bool have = readBack + 2.0 < static_cast<double>(filled);
                const double back = std::max(1.0, readBack);
                const auto whole = static_cast<std::size_t>(back);
                const auto frac = static_cast<float>(back - static_cast<double>(whole));
                const std::size_t i0 = (write + size - whole) % size;
                const std::size_t i1 = (i0 + size - 1) % size;
                const auto fade = static_cast<float>(std::min(1.0, 20.0 * (1.0 - t))); // the last 5 %: to silence
                left[i] = have ? fade * (histL[i0] + frac * (histL[i1] - histL[i0])) : 0.0f;
                right[i] = have ? fade * (histR[i0] + frac * (histR[i1] - histR[i0])) : 0.0f;
                if (t >= 1.0)
                    current = Stage::Finished;
            }
            break;
        }
        case Kind::FilterSweep:
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const double t = std::min(1.0, elapsed / length);
                double hz = 0.0;
                float gain = 1.0f;
                if (current == Stage::Held)
                    hz = 20.0 * std::pow(75.0, t); // 20 Hz -> 1.5 kHz
                else
                {
                    hz = 1500.0 * std::pow(10.0, t); // -> 15 kHz
                    gain = static_cast<float>(1.0 - t);
                }
                elapsed += 1.0;
                left[i] = gain * hp(0, left[i], hz);
                right[i] = gain * hp(1, right[i], hz);
                if (current == Stage::Released && t >= 1.0)
                    current = Stage::Finished;
            }
            break;
        }
        case Kind::Count:
            break;
    }
}

} // namespace devildj::dsp::fx
