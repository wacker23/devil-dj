#include "dsp/fx/BeatFx.h"

#include "dsp/Stretcher.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace devildj::dsp::fx
{

namespace
{
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kMaxDelaySeconds = 16.0;

// tanh-like, |x| -> 1 smoothly (a Pade approximation, exact enough for a clipper).
inline float fastTanh(float x) noexcept
{
    if (x > 3.0f)
        return 1.0f;
    if (x < -3.0f)
        return -1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// A soft clipper that leaves small signals alone and never exceeds `limit`.
inline float softClip(float x, float limit = 1.5f) noexcept
{
    return limit * fastTanh(x / limit);
}

double clampBpm(double bpm) noexcept
{
    return std::clamp(bpm > 0.0 ? bpm : 120.0, 20.0, 300.0);
}

double beatSamples(double beats, double bpm, double sampleRate) noexcept
{
    return beats * 60.0 / clampBpm(bpm) * sampleRate;
}

// A delay line of floats that is "cleared" in O(1): what was never written reads as silence.
class DelayLine
{
public:
    void prepare(std::size_t length)
    {
        buffer.assign(std::max<std::size_t>(length, 4), 0.0f);
        reset();
    }
    void reset() noexcept
    {
        write = 0;
        filled = 0;
    }
    void push(float v) noexcept
    {
        buffer[write] = v;
        write = write + 1 == buffer.size() ? 0 : write + 1;
        filled = std::min(filled + 1, buffer.size());
    }
    // The sample `delay` samples ago (fractional, linear), 1 <= delay <= length - 2.
    float tap(double delay) const noexcept
    {
        delay = std::clamp(delay, 1.0, static_cast<double>(buffer.size() - 2));
        if (delay > static_cast<double>(filled))
            return 0.0f;
        const auto whole = static_cast<std::size_t>(delay);
        const auto frac = static_cast<float>(delay - static_cast<double>(whole));
        const std::size_t n = buffer.size();
        const std::size_t i0 = (write + n - whole) % n;
        const std::size_t i1 = (i0 + n - 1) % n;
        return buffer[i0] + frac * (buffer[i1] - buffer[i0]);
    }
    std::size_t length() const noexcept { return buffer.size(); }
    std::size_t available() const noexcept { return filled; }

private:
    std::vector<float> buffer;
    std::size_t write = 0, filled = 0;
};

// One-pole smoother for times that follow the tempo (no zipper when the tempo or beats change).
struct Smoothed
{
    double value = -1.0;
    double next(double target, double coefficient) noexcept
    {
        value = value < 0.0 ? target : value + coefficient * (target - value);
        return value;
    }
};

// A TPT state-variable filter (Zavalishin): low-pass / band-pass outputs.
struct Svf
{
    double ic1 = 0.0, ic2 = 0.0;
    void reset() noexcept { ic1 = ic2 = 0.0; }
    // g = tan(pi fc / fs), k = 1 / Q
    void process(double x, double g, double k, double& low, double& band) noexcept
    {
        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        low = v2;
        band = v1;
    }
};

// The phase (0..1) of an LFO of one cycle per `beats` at the sample `i` of the block (from the master beat).
double lfoPhase(const BeatContext& beat, int i, double beats, double sampleRate) noexcept
{
    const double perSample = beat.beatsPerSample > 0.0 ? beat.beatsPerSample : clampBpm(beat.bpm) / (60.0 * sampleRate);
    const double b = beat.beatAtBlockStart + i * perSample;
    const double cycles = b / std::max(beats, 1.0 / 64.0);
    return cycles - std::floor(cycles);
}

// Common bits: sample rate, beats, parameters with names and defaults.
template <int N>
class Base : public Effect
{
public:
    Base(const char* effectName, std::array<EffectParam, N> parameters, bool tail)
        : label(effectName), info(parameters), tailed(tail)
    {
        for (int i = 0; i < N; ++i)
            values[static_cast<std::size_t>(i)] = info[static_cast<std::size_t>(i)].defaultValue;
    }
    const char* name() const noexcept override { return label; }
    bool hasTail() const noexcept override { return tailed; }
    void setBeats(double b) noexcept override { beats = std::clamp(b, 1.0 / 16.0, 16.0); }
    int numParams() const noexcept override { return N; }
    EffectParam param(int index) const noexcept override
    {
        return index >= 0 && index < N ? info[static_cast<std::size_t>(index)] : EffectParam{};
    }
    void setParam(int index, float value) noexcept override
    {
        if (index >= 0 && index < N)
            values[static_cast<std::size_t>(index)] = std::clamp(value, 0.0f, 1.0f);
    }

protected:
    float p(int index) const noexcept { return values[static_cast<std::size_t>(index)]; }
    const char* label;
    std::array<EffectParam, N> info;
    std::array<float, N> values{};
    bool tailed;
    double sampleRate = 48000.0;
    double beats = 1.0;
};

template <>
class Base<0> : public Effect
{
public:
    Base(const char* effectName, std::array<EffectParam, 0>, bool tail)
        : label(effectName), tailed(tail)
    {
    }
    const char* name() const noexcept override { return label; }
    bool hasTail() const noexcept override { return tailed; }
    void setBeats(double b) noexcept override { beats = std::clamp(b, 1.0 / 16.0, 16.0); }

protected:
    const char* label;
    bool tailed;
    double sampleRate = 48000.0;
    double beats = 1.0;
};

// ---- Echo / Ping Pong ----

class Echo final : public Base<2>
{
public:
    explicit Echo(bool pingPong)
        : Base(pingPong ? "Ping Pong" : "Echo", {{{"Feedback", 0.5f}, {"Tone", 0.6f}}}, true), bounce(pingPong)
    {
    }
    void prepare(double rate, int) override
    {
        sampleRate = rate;
        for (auto* d : {&lineL, &lineR})
            d->prepare(static_cast<std::size_t>(kMaxDelaySeconds * rate) + 4);
        reset();
    }
    void reset() noexcept override
    {
        lineL.reset();
        lineR.reset();
        lpL = lpR = 0.0f;
        time.value = -1.0;
    }
    void process(float* l, float* r, int n, const BeatContext& beat) noexcept override
    {
        const auto feedback = 0.9f * p(0);
        const double cutoff = 500.0 * std::pow(40.0, static_cast<double>(p(1))); // 500 Hz .. 20 kHz
        const auto a = static_cast<float>(1.0 - std::exp(-kTwoPi * std::min(cutoff, 0.45 * sampleRate) / sampleRate));
        const double target = std::clamp(beatSamples(beats, beat.bpm, sampleRate), 1.0, lineL.length() - 2.0);
        const double smooth = 1.0 - std::exp(-1.0 / (0.05 * sampleRate));
        for (int i = 0; i < n; ++i)
        {
            const double d = time.next(target, smooth);
            const float yl = lineL.tap(d), yr = lineR.tap(d);
            lpL += a * (yl - lpL);
            lpR += a * (yr - lpR);
            if (bounce) // mono in on the left; each side feeds the other
            {
                lineL.push(softClip(0.5f * (l[i] + r[i]) + feedback * lpR));
                lineR.push(softClip(feedback * lpL));
            }
            else
            {
                lineL.push(softClip(l[i] + feedback * lpL));
                lineR.push(softClip(r[i] + feedback * lpR));
            }
            l[i] = yl;
            r[i] = yr;
        }
    }

private:
    bool bounce;
    DelayLine lineL, lineR;
    float lpL = 0.0f, lpR = 0.0f;
    Smoothed time;
};

// ---- Reverb: 8-line FDN, Householder feedback ----

class Reverb final : public Base<3>
{
public:
    Reverb() : Base("Reverb", {{{"Decay", 0.6f}, {"Damping", 0.4f}, {"Size", 0.5f}}}, true) {}
    void prepare(double rate, int) override
    {
        sampleRate = rate;
        for (auto& line : lines)
            line.prepare(static_cast<std::size_t>(0.25 * rate) + 8);
        reset();
    }
    void reset() noexcept override
    {
        for (auto& line : lines)
            line.reset();
        damp.fill(0.0f);
    }
    void process(float* l, float* r, int n, const BeatContext&) noexcept override
    {
        static constexpr std::array<double, 8> base{0.0297, 0.0371, 0.0411, 0.0437, 0.0533, 0.0599, 0.0677, 0.0743};
        const double size = 0.6 + 1.6 * p(2);
        const auto g = static_cast<float>(0.2 + 0.77 * p(0));                // loop gain per pass
        const auto a = static_cast<float>(1.0 - 0.85 * static_cast<double>(p(1))); // damping low-pass
        std::array<double, 8> delays{};
        for (std::size_t k = 0; k < 8; ++k)
            delays[k] = std::min(base[k] * size * sampleRate, static_cast<double>(lines[k].length() - 2));
        for (int i = 0; i < n; ++i)
        {
            std::array<float, 8> y{};
            float sum = 0.0f;
            for (std::size_t k = 0; k < 8; ++k)
            {
                const float v = lines[k].tap(delays[k]);
                damp[k] += a * (v - damp[k]);
                y[k] = damp[k];
                sum += y[k];
            }
            const float in = 0.35f * (l[i] + r[i]);
            float outL = 0.0f, outR = 0.0f;
            for (std::size_t k = 0; k < 8; ++k)
            {
                const float fed = y[k] - 0.25f * sum; // Householder: I - 2/N x 1 1^T (orthogonal)
                lines[k].push(softClip(g * fed + (k % 2 == 0 ? in : -in)));
                (k % 2 == 0 ? outL : outR) += y[k];
            }
            l[i] = softClip(0.35f * outL, 1.8f); // four lines per side: kept under +6 dBFS
            r[i] = softClip(0.35f * outR, 1.8f);
        }
    }

private:
    std::array<DelayLine, 8> lines;
    std::array<float, 8> damp{};
};

// ---- Flanger ----

class Flanger final : public Base<2>
{
public:
    Flanger() : Base("Flanger", {{{"Depth", 0.7f}, {"Feedback", 0.5f}}}, false) {}
    void prepare(double rate, int) override
    {
        sampleRate = rate;
        lineL.prepare(static_cast<std::size_t>(0.02 * rate) + 8);
        lineR.prepare(static_cast<std::size_t>(0.02 * rate) + 8);
        reset();
    }
    void reset() noexcept override
    {
        lineL.reset();
        lineR.reset();
        fbL = fbR = 0.0f;
    }
    void process(float* l, float* r, int n, const BeatContext& beat) noexcept override
    {
        const double depthMs = 0.5 + 7.5 * p(0);
        const auto feedback = 0.85f * p(1);
        for (int i = 0; i < n; ++i)
        {
            const double phase = lfoPhase(beat, i, beats, sampleRate);
            const double lfoL = 0.5 - 0.5 * std::cos(kTwoPi * phase);
            const double lfoR = 0.5 - 0.5 * std::cos(kTwoPi * (phase + 0.25));
            const double dl = (0.5 + (depthMs - 0.5) * lfoL) * 0.001 * sampleRate;
            const double dr = (0.5 + (depthMs - 0.5) * lfoR) * 0.001 * sampleRate;
            const float x0 = l[i], x1 = r[i];
            lineL.push(softClip(x0 + feedback * fbL));
            lineR.push(softClip(x1 + feedback * fbR));
            fbL = lineL.tap(std::max(1.0, dl));
            fbR = lineR.tap(std::max(1.0, dr));
            l[i] = 0.5f * (x0 + fbL);
            r[i] = 0.5f * (x1 + fbR);
        }
    }

private:
    DelayLine lineL, lineR;
    float fbL = 0.0f, fbR = 0.0f;
};

// ---- Phaser ----

class Phaser final : public Base<2>
{
public:
    Phaser() : Base("Phaser", {{{"Depth", 0.7f}, {"Feedback", 0.4f}}}, false) {}
    void prepare(double rate, int) override
    {
        sampleRate = rate;
        reset();
    }
    void reset() noexcept override
    {
        for (auto& s : state)
            s.fill(0.0f);
        fb.fill(0.0f);
    }
    void process(float* l, float* r, int n, const BeatContext& beat) noexcept override
    {
        const double depth = p(0);
        const auto feedback = 0.8f * p(1);
        float* io[2] = {l, r};
        for (int i = 0; i < n; ++i)
        {
            const double phase = lfoPhase(beat, i, beats, sampleRate);
            for (int c = 0; c < 2; ++c)
            {
                const double lfo = 0.5 - 0.5 * std::cos(kTwoPi * (phase + 0.25 * c));
                const double fc = 200.0 * std::pow(2.0, 6.0 * depth * lfo); // 200 Hz .. 12.8 kHz
                const double t = std::tan(std::numbers::pi * std::min(fc, 0.45 * sampleRate) / sampleRate);
                const auto coef = static_cast<float>((t - 1.0) / (t + 1.0));
                const float x = io[c][i];
                float v = softClip(x + feedback * fb[static_cast<std::size_t>(c)], 1.0f);
                for (auto& z : state[static_cast<std::size_t>(c)]) // first-order all-passes
                {
                    const float out = coef * v + z;
                    z = v - coef * out;
                    v = out;
                }
                fb[static_cast<std::size_t>(c)] = v;
                io[c][i] = 0.5f * (x + v);
            }
        }
    }

private:
    std::array<std::array<float, 6>, 2> state{};
    std::array<float, 2> fb{};
};

// ---- Filter LFO ----

class FilterLfo final : public Base<2>
{
public:
    FilterLfo() : Base("Filter LFO", {{{"Depth", 0.8f}, {"Resonance", 0.4f}}}, false) {}
    void prepare(double rate, int) override
    {
        sampleRate = rate;
        reset();
    }
    void reset() noexcept override
    {
        for (auto& f : filters)
            f.reset();
    }
    void process(float* l, float* r, int n, const BeatContext& beat) noexcept override
    {
        const double depth = p(0);
        const double k = 2.0 - 1.8 * p(1); // 1/Q: 2 (no peak) .. 0.2
        float* io[2] = {l, r};
        for (int i = 0; i < n; ++i)
        {
            const double phase = lfoPhase(beat, i, beats, sampleRate);
            const double tri = 1.0 - std::abs(2.0 * phase - 1.0); // 0 -> 1 -> 0 over the beat length
            const double fc = 12000.0 * std::pow(2.0, -6.0 * depth * (1.0 - tri)); // down to ~190 Hz
            const double g = std::tan(std::numbers::pi * std::min(fc, 0.45 * sampleRate) / sampleRate);
            for (int c = 0; c < 2; ++c)
            {
                double low = 0.0, band = 0.0;
                filters[static_cast<std::size_t>(c)].process(io[c][i], g, k, low, band);
                io[c][i] = softClip(static_cast<float>(low));
            }
        }
    }

private:
    std::array<Svf, 2> filters;
};

// ---- Roll: quantized buffer repeat ----

class Roll final : public Base<0>
{
public:
    Roll() : Base("Roll", {}, false) {}
    void prepare(double rate, int) override
    {
        sampleRate = rate;
        const auto n = static_cast<std::size_t>(kMaxDelaySeconds * rate) + 4;
        bufL.assign(n, 0.0f);
        bufR.assign(n, 0.0f);
        reset();
    }
    void reset() noexcept override
    {
        stage = Stage::Waiting;
        lastIndex = -1;
        length = recorded = position = 0;
    }
    void setBeats(double b) noexcept override
    {
        const double clamped = std::clamp(b, 1.0 / 16.0, 16.0);
        if (clamped != beats)
            reset(); // a new length: from its next boundary
        beats = clamped;
    }
    void process(float* l, float* r, int n, const BeatContext& beat) noexcept override
    {
        const double perSample = beat.beatsPerSample > 0.0 ? beat.beatsPerSample : clampBpm(beat.bpm) / (60.0 * sampleRate);
        const int fade = static_cast<int>(0.002 * sampleRate);
        for (int i = 0; i < n; ++i)
        {
            const double b = beat.beatAtBlockStart + i * perSample;
            const auto index = static_cast<long long>(std::floor(b / beats));
            if (stage == Stage::Waiting && lastIndex >= 0 && index != lastIndex) // a boundary: record from here
            {
                stage = Stage::Recording;
                length = static_cast<int>(std::min<double>(beatSamples(beats, beat.bpm, sampleRate),
                                                           static_cast<double>(bufL.size())));
                length = std::max(length, 2 * fade + 2);
                recorded = 0;
            }
            lastIndex = index;
            if (stage == Stage::Recording)
            {
                bufL[static_cast<std::size_t>(recorded)] = l[i];
                bufR[static_cast<std::size_t>(recorded)] = r[i];
                if (++recorded >= length)
                {
                    stage = Stage::Repeating;
                    position = 0;
                }
            }
            else if (stage == Stage::Repeating)
            {
                // Each repeat: the recorded slice, its first 2 ms crossfaded with what follows its end (the input
                // there was continuous, so the seam is smooth).
                float vl = bufL[static_cast<std::size_t>(position)], vr = bufR[static_cast<std::size_t>(position)];
                if (position < fade)
                {
                    const float t = static_cast<float>(position) / static_cast<float>(fade);
                    const auto tailIndex = static_cast<std::size_t>(length - fade + position);
                    vl = t * vl + (1.0f - t) * bufL[tailIndex];
                    vr = t * vr + (1.0f - t) * bufR[tailIndex];
                }
                l[i] = vl;
                r[i] = vr;
                if (++position >= length - fade)
                    position = 0;
            }
            // Waiting: the input passes through until the boundary.
        }
    }

private:
    enum class Stage
    {
        Waiting,
        Recording,
        Repeating,
    };
    Stage stage = Stage::Waiting;
    long long lastIndex = -1;
    int length = 0, recorded = 0, position = 0;
    std::vector<float> bufL, bufR;
};

// ---- Trans: beat-synced gate ----

class Trans final : public Base<1>
{
public:
    Trans() : Base("Trans", {{{"Duty", 0.5f}}}, false) {}
    void prepare(double rate, int) override
    {
        sampleRate = rate;
        reset();
    }
    void reset() noexcept override { gain = 1.0f; }
    void process(float* l, float* r, int n, const BeatContext& beat) noexcept override
    {
        const double duty = 0.1 + 0.8 * p(0);
        const auto step = static_cast<float>(1.0 / (0.002 * sampleRate)); // 2 ms edges
        for (int i = 0; i < n; ++i)
        {
            const float target = lfoPhase(beat, i, beats, sampleRate) < duty ? 1.0f : 0.0f;
            gain = gain < target ? std::min(target, gain + step) : std::max(target, gain - step);
            l[i] *= gain;
            r[i] *= gain;
        }
    }

private:
    float gain = 1.0f;
};

// ---- Spiral: an echo whose time shortens ----

class Spiral final : public Base<2>
{
public:
    Spiral() : Base("Spiral", {{{"Feedback", 0.7f}, {"Speed", 0.5f}}}, true) {}
    void prepare(double rate, int) override
    {
        sampleRate = rate;
        lineL.prepare(static_cast<std::size_t>(kMaxDelaySeconds * rate) + 4);
        lineR.prepare(static_cast<std::size_t>(kMaxDelaySeconds * rate) + 4);
        reset();
    }
    void reset() noexcept override
    {
        lineL.reset();
        lineR.reset();
        delay = -1.0;
    }
    void process(float* l, float* r, int n, const BeatContext& beat) noexcept override
    {
        const auto feedback = 0.9f * p(0);
        const double full = std::clamp(beatSamples(beats, beat.bpm, sampleRate), 64.0, lineL.length() - 2.0);
        const double shrink = 0.05 + 0.45 * p(1); // the delay loses this share of itself per second
        const double perSample = std::pow(1.0 - shrink, 1.0 / sampleRate);
        if (delay < 0.0)
            delay = full;
        for (int i = 0; i < n; ++i)
        {
            delay *= perSample;
            if (delay < 0.004 * sampleRate) // down to 4 ms: round again from the beat length
                delay = full;
            const float yl = lineL.tap(delay), yr = lineR.tap(delay);
            lineL.push(softClip(l[i] + feedback * yl));
            lineR.push(softClip(r[i] + feedback * yr));
            l[i] = yl;
            r[i] = yr;
        }
    }

private:
    DelayLine lineL, lineR;
    double delay = -1.0;
};

// ---- Bit Crush ----

class BitCrush final : public Base<2>
{
public:
    BitCrush() : Base("Bit Crush", {{{"Bits", 0.5f}, {"Downsample", 0.3f}}}, false) {}
    void prepare(double rate, int) override
    {
        sampleRate = rate;
        reset();
    }
    void reset() noexcept override
    {
        holdL = holdR = 0.0f;
        counter = 0.0;
    }
    void process(float* l, float* r, int n, const BeatContext&) noexcept override
    {
        const double bits = 16.0 - 14.0 * p(0); // 16 .. 2 bits
        const auto levels = static_cast<float>(std::pow(2.0, bits - 1.0));
        const double factor = 1.0 + 31.0 * p(1) * p(1); // hold each sample 1 .. 32 times
        for (int i = 0; i < n; ++i)
        {
            counter += 1.0;
            if (counter >= factor)
            {
                counter -= factor;
                holdL = std::round(std::clamp(l[i], -1.0f, 1.0f) * levels) / levels;
                holdR = std::round(std::clamp(r[i], -1.0f, 1.0f) * levels) / levels;
            }
            l[i] = holdL;
            r[i] = holdR;
        }
    }

private:
    float holdL = 0.0f, holdR = 0.0f;
    double counter = 0.0;
};

// ---- Noise sweep ----

class Noise final : public Base<2>
{
public:
    Noise() : Base("Noise", {{{"Level", 0.5f}, {"Resonance", 0.5f}}}, false) {}
    void prepare(double rate, int) override
    {
        sampleRate = rate;
        reset();
    }
    void reset() noexcept override
    {
        for (auto& f : filters)
            f.reset();
    }
    void process(float* l, float* r, int n, const BeatContext& beat) noexcept override
    {
        const auto level = static_cast<float>(0.6 * p(0));
        const double k = 1.4 - 1.2 * p(1);
        float* io[2] = {l, r};
        for (int i = 0; i < n; ++i)
        {
            const double phase = lfoPhase(beat, i, beats, sampleRate);
            const double fc = 300.0 * std::pow(2.0, 5.5 * phase); // rises over the beat length: 300 Hz .. 13.6 kHz
            const double g = std::tan(std::numbers::pi * std::min(fc, 0.45 * sampleRate) / sampleRate);
            for (int c = 0; c < 2; ++c)
            {
                seed = seed * 1664525u + 1013904223u; // a small LCG: white noise
                const double white = static_cast<double>(seed >> 8) / 8388608.0 - 1.0;
                double low = 0.0, band = 0.0;
                filters[static_cast<std::size_t>(c)].process(white, g, k, low, band);
                io[c][i] = io[c][i] + level * softClip(static_cast<float>(band), 1.0f);
            }
        }
    }

private:
    std::array<Svf, 2> filters;
    std::uint32_t seed = 12345u;
};

// ---- Pitch (the stretcher, transposing at rate 1) ----

class Pitch final : public Base<1>
{
public:
    Pitch() : Base("Pitch", {{{"Semitones", 0.5f}}}, false) {}
    void prepare(double rate, int maxBlock) override
    {
        sampleRate = rate;
        if (stretcher == nullptr)
            stretcher = makeSignalsmithStretcher();
        stretcher->prepare(2, rate);
        prime.assign(static_cast<std::size_t>(stretcher->seekLength()), 0.0f);
        outL.assign(static_cast<std::size_t>(std::max(1, maxBlock)), 0.0f);
        outR.assign(outL.size(), 0.0f);
        reset();
    }
    void reset() noexcept override
    {
        if (stretcher == nullptr)
            return;
        stretcher->reset();
        const float* in[] = {prime.data(), prime.data()}; // prime with silence
        stretcher->seek(in, static_cast<int>(prime.size()), 1.0);
        shown = 1000.0;
    }
    void process(float* l, float* r, int n, const BeatContext&) noexcept override
    {
        const double semitones = std::round(-12.0 + 24.0 * p(0));
        if (semitones != shown)
        {
            stretcher->setTransposeSemitones(semitones);
            shown = semitones;
        }
        for (int done = 0; done < n;)
        {
            const int m = std::min(n - done, static_cast<int>(outL.size()));
            const float* in[] = {l + done, r + done};
            float* out[] = {outL.data(), outR.data()};
            stretcher->process(in, m, out, m);
            for (int i = 0; i < m; ++i)
            {
                l[done + i] = softClip(outL[static_cast<std::size_t>(i)]);
                r[done + i] = softClip(outR[static_cast<std::size_t>(i)]);
            }
            done += m;
        }
    }

private:
    std::unique_ptr<Stretcher> stretcher;
    std::vector<float> prime, outL, outR;
    double shown = 1000.0;
};

// ---- Vinyl brake ----

class VinylBrake final : public Base<0>
{
public:
    VinylBrake() : Base("Vinyl Brake", {}, false) {}
    void prepare(double rate, int) override
    {
        sampleRate = rate;
        lineL.prepare(static_cast<std::size_t>(kMaxDelaySeconds * rate) + 4);
        lineR.prepare(static_cast<std::size_t>(kMaxDelaySeconds * rate) + 4);
        reset();
    }
    void reset() noexcept override
    {
        lineL.reset();
        lineR.reset();
        lag = 0.0;
        elapsed = 0.0;
        speed = 1.0;
    }
    void process(float* l, float* r, int n, const BeatContext& beat) noexcept override
    {
        // From switch-on, the sound is read ever slower (1 -> 0 over the beat length): it falls behind the input.
        const double total = std::max(1.0, beatSamples(beats, beat.bpm, sampleRate));
        for (int i = 0; i < n; ++i)
        {
            lineL.push(l[i]);
            lineR.push(r[i]);
            speed = std::max(0.0, 1.0 - elapsed / total);
            elapsed += 1.0;
            lag += 1.0 - speed;
            if (speed <= 0.0 || lag + 2.0 >= static_cast<double>(lineL.length()))
            {
                l[i] = r[i] = 0.0f;
                continue;
            }
            l[i] = lineL.tap(lag + 1.0);
            r[i] = lineR.tap(lag + 1.0);
        }
    }

private:
    DelayLine lineL, lineR;
    double lag = 0.0, elapsed = 0.0, speed = 1.0;
};

// ---- Freeze (helix): hold the last beat length ----

class Freeze final : public Base<0>
{
public:
    Freeze() : Base("Freeze", {}, false) {}
    void prepare(double rate, int) override
    {
        sampleRate = rate;
        const auto n = static_cast<std::size_t>(4.0 * rate) + 4; // up to 4 s held
        historyL.prepare(n);
        historyR.prepare(n);
        holdL.assign(n, 0.0f);
        holdR.assign(n, 0.0f);
        reset();
    }
    void reset() noexcept override
    {
        frozen = false;
        length = position = 0;
        historyL.reset();
        historyR.reset();
    }
    void feed(const float* l, const float* r, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            historyL.push(l[i]);
            historyR.push(r[i]);
        }
    }
    void process(float* l, float* r, int n, const BeatContext& beat) noexcept override
    {
        const int fade = static_cast<int>(0.004 * sampleRate);
        if (!frozen)
        {
            // From switch-on: the input passes while the first beat length of it is taken, then that is held.
            feed(l, r, n);
            const auto wanted = std::clamp<std::size_t>(
                static_cast<std::size_t>(beatSamples(std::min(beats, 4.0), beat.bpm, sampleRate)),
                static_cast<std::size_t>(4 * fade), holdL.size() - 1);
            if (historyL.available() < wanted)
                return; // not all of it yet: pass the input
            length = static_cast<int>(wanted);
            for (int k = 0; k < length; ++k)
            {
                holdL[static_cast<std::size_t>(k)] = historyL.tap(static_cast<double>(length - k));
                holdR[static_cast<std::size_t>(k)] = historyR.tap(static_cast<double>(length - k));
            }
            frozen = true;
            position = 0;
            return; // this block passes; the held loop starts with the next
        }
        for (int i = 0; i < n; ++i)
        {
            float vl = holdL[static_cast<std::size_t>(position)], vr = holdR[static_cast<std::size_t>(position)];
            const int toEnd = length - position;
            if (toEnd <= fade) // crossfade the end into the start
            {
                const float t = static_cast<float>(toEnd) / static_cast<float>(fade);
                const auto start = static_cast<std::size_t>(fade - toEnd);
                vl = t * vl + (1.0f - t) * holdL[start];
                vr = t * vr + (1.0f - t) * holdR[start];
            }
            l[i] = vl;
            r[i] = vr;
            if (++position >= length)
                position = fade; // the crossfade already played the first `fade` samples
        }
    }

private:
    DelayLine historyL, historyR;
    std::vector<float> holdL, holdR;
    bool frozen = false;
    int length = 0, position = 0;
};

} // namespace

const char* effectTypeName(EffectType type) noexcept
{
    switch (type)
    {
        case EffectType::Echo:
            return "Echo";
        case EffectType::PingPong:
            return "Ping Pong";
        case EffectType::Reverb:
            return "Reverb";
        case EffectType::Flanger:
            return "Flanger";
        case EffectType::Phaser:
            return "Phaser";
        case EffectType::FilterLfo:
            return "Filter LFO";
        case EffectType::Roll:
            return "Roll";
        case EffectType::Trans:
            return "Trans";
        case EffectType::Spiral:
            return "Spiral";
        case EffectType::BitCrush:
            return "Bit Crush";
        case EffectType::Noise:
            return "Noise";
        case EffectType::Pitch:
            return "Pitch";
        case EffectType::VinylBrake:
            return "Vinyl Brake";
        case EffectType::Freeze:
            return "Freeze";
        case EffectType::Count:
            break;
    }
    return "";
}

std::unique_ptr<Effect> makeEffect(EffectType type)
{
    switch (type)
    {
        case EffectType::Echo:
            return std::make_unique<Echo>(false);
        case EffectType::PingPong:
            return std::make_unique<Echo>(true);
        case EffectType::Reverb:
            return std::make_unique<Reverb>();
        case EffectType::Flanger:
            return std::make_unique<Flanger>();
        case EffectType::Phaser:
            return std::make_unique<Phaser>();
        case EffectType::FilterLfo:
            return std::make_unique<FilterLfo>();
        case EffectType::Roll:
            return std::make_unique<Roll>();
        case EffectType::Trans:
            return std::make_unique<Trans>();
        case EffectType::Spiral:
            return std::make_unique<Spiral>();
        case EffectType::BitCrush:
            return std::make_unique<BitCrush>();
        case EffectType::Noise:
            return std::make_unique<Noise>();
        case EffectType::Pitch:
            return std::make_unique<Pitch>();
        case EffectType::VinylBrake:
            return std::make_unique<VinylBrake>();
        case EffectType::Freeze:
            return std::make_unique<Freeze>();
        case EffectType::Count:
            break;
    }
    return nullptr;
}

} // namespace devildj::dsp::fx
