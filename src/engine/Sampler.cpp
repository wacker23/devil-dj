#include "engine/Sampler.h"

#include <algorithm>
#include <cmath>

namespace devildj::engine
{

Sampler::Sampler() = default;
Sampler::~Sampler() = default;

void Sampler::prepare(double rate, int maxBlockSize)
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    fadeSamples = std::max(1, static_cast<int>(std::lround(0.002 * sampleRate)));
    maxBlock = std::max(1, maxBlockSize);
    const auto maxIn = static_cast<std::size_t>(2 * maxBlock + 64); // up to ~2x faster when synced
    for (auto& v : voices)
    {
        if (v.stretcher == nullptr)
            v.stretcher = dsp::makeSignalsmithStretcher();
        v.stretcher->prepare(2, sampleRate);
    }
    const auto seek = static_cast<std::size_t>(voices[0].stretcher->seekLength());
    inL.assign(maxIn, 0.0f);
    inR.assign(maxIn, 0.0f);
    outL.assign(static_cast<std::size_t>(maxBlock), 0.0f);
    outR.assign(static_cast<std::size_t>(maxBlock), 0.0f);
    seekL.assign(seek, 0.0f);
    seekR.assign(seek, 0.0f);
    reset();
}

void Sampler::reset() noexcept
{
    for (auto& v : voices)
    {
        v.active = false;
        v.slot = -1;
        v.buffer = nullptr;
    }
}

BufferId Sampler::load(int index, const TrackBuffer* buffer, BufferId id, double bpm) noexcept
{
    if (index < 0 || index >= kSlots)
        return id; // give it straight back
    auto& s = slots[static_cast<std::size_t>(index)];
    for (auto& v : voices) // nothing may read the old buffer any more
        if (v.active && v.slot == index)
        {
            v.active = false;
            v.buffer = nullptr;
        }
    const BufferId old = s.id;
    s.buffer = buffer;
    s.id = buffer != nullptr ? id : 0;
    s.bpm = bpm > 0.0 ? bpm : 0.0;
    return old;
}

void Sampler::setMode(int index, Mode mode) noexcept
{
    if (index >= 0 && index < kSlots)
        slots[static_cast<std::size_t>(index)].mode = mode;
}

void Sampler::setGain(int index, float gain) noexcept
{
    if (index >= 0 && index < kSlots)
        slots[static_cast<std::size_t>(index)].gain = std::clamp(gain, 0.0f, 2.0f);
}

void Sampler::setSync(int index, bool on) noexcept
{
    if (index >= 0 && index < kSlots)
        slots[static_cast<std::size_t>(index)].sync = on;
}

Sampler::Voice* Sampler::voiceFor(int index) noexcept
{
    for (auto& v : voices)
        if (v.active && v.slot == index && v.fadeOut < 0)
            return &v;
    return nullptr;
}

Sampler::Voice& Sampler::freeVoice() noexcept
{
    for (auto& v : voices)
        if (!v.active)
            return v;
    // All busy: the oldest goes (it would click; it is cut short with the 2 ms fade it is given below).
    Voice* oldest = &voices[0];
    for (auto& v : voices)
        if (v.started < oldest->started)
            oldest = &v;
    return *oldest;
}

void Sampler::startVoice(Voice& v, int index, int delay) noexcept
{
    const auto& s = slots[static_cast<std::size_t>(index)];
    v.active = true;
    v.slot = index;
    v.buffer = s.buffer;
    v.delay = std::max(0, delay);
    v.position = 0.0;
    v.primed = false;
    v.fadeIn = fadeSamples;
    v.fadeOut = -1;
    v.started = ++counter;
}

void Sampler::press(int index, int delay) noexcept
{
    if (index < 0 || index >= kSlots)
        return;
    const auto& s = slots[static_cast<std::size_t>(index)];
    if (s.buffer == nullptr || s.buffer->numSamples() <= 0)
        return;
    if (auto* playing = voiceFor(index))
    {
        if (s.mode == Mode::Loop) // a loop: the press stops it
        {
            playing->fadeOut = fadeSamples;
            return;
        }
        playing->fadeOut = fadeSamples; // one-shot again: the old one fades, a new one starts
    }
    startVoice(freeVoice(), index, delay);
}

void Sampler::releasePad(int index) noexcept
{
    if (index < 0 || index >= kSlots || slots[static_cast<std::size_t>(index)].mode != Mode::Gate)
        return;
    stop(index);
}

void Sampler::stop(int index) noexcept
{
    for (auto& v : voices)
        if (v.active && v.slot == index && v.fadeOut < 0)
            v.fadeOut = v.delay > 0 ? 0 : fadeSamples; // not started yet: just drop it
}

void Sampler::stopAll() noexcept
{
    for (int i = 0; i < kSlots; ++i)
        stop(i);
}

double Sampler::estimateLoopBpm(double seconds, double masterBpm) noexcept
{
    if (!(seconds > 0.0) || !(masterBpm > 0.0))
        return 0.0;
    const double beatsAtMaster = seconds * masterBpm / 60.0;
    const double beats = std::pow(2.0, std::max(0.0, std::round(std::log2(std::max(beatsAtMaster, 1.0)))));
    const double bpm = beats * 60.0 / seconds;
    return bpm >= 60.0 && bpm <= 200.0 ? bpm : 0.0;
}

bool Sampler::isPlaying(int index) const noexcept
{
    for (const auto& v : voices)
        if (v.active && v.slot == index && v.fadeOut < 0)
            return true;
    return false;
}

double Sampler::progress(int index) const noexcept
{
    for (const auto& v : voices)
        if (v.active && v.slot == index && v.fadeOut < 0 && v.buffer != nullptr && v.buffer->numSamples() > 0)
            return std::clamp(v.position / v.buffer->numSamples(), 0.0, 1.0);
    return -1.0;
}

float Sampler::frame(const Voice& v, int channel, std::int64_t index) const noexcept
{
    const auto len = static_cast<std::int64_t>(v.buffer->numSamples());
    const bool loop = slots[static_cast<std::size_t>(v.slot)].mode == Mode::Loop;
    if (loop)
        index = ((index % len) + len) % len;
    else if (index < 0 || index >= len)
        return 0.0f;
    return v.buffer->audio.getReadPointer(channel)[index];
}

void Sampler::render(float* left, float* right, int numSamples, double masterBpm) noexcept
{
    for (int done = 0; done < numSamples; done += maxBlock)
    {
        const int n = std::min(maxBlock, numSamples - done);
        for (auto& v : voices)
            if (v.active)
                renderVoice(v, left + done, right + done, n, masterBpm);
    }
}

void Sampler::renderVoice(Voice& v, float* left, float* right, int numSamples, double masterBpm) noexcept
{
    const auto& s = slots[static_cast<std::size_t>(v.slot)];
    if (v.buffer == nullptr || v.buffer != s.buffer)
    {
        v.active = false;
        return;
    }
    int start = 0;
    if (v.delay > 0) // a quantized start later in this block, or after it
    {
        if (v.delay >= numSamples)
        {
            v.delay -= numSamples;
            if (v.fadeOut >= 0)
                v.active = false;
            return;
        }
        start = v.delay;
        v.delay = 0;
    }
    if (v.fadeOut == 0)
    {
        v.active = false;
        return;
    }
    const int n = numSamples - start;
    const double len = v.buffer->numSamples();
    const bool loop = s.mode == Mode::Loop;
    // Sync: input samples per output sample = master tempo / the sample's tempo.
    const double rate = s.sync && s.bpm > 0.0 && masterBpm > 0.0 ? std::clamp(masterBpm / s.bpm, 0.5, 2.0) : 1.0;
    const bool stretched = std::abs(rate - 1.0) > 1e-9;
    if (stretched != v.stretched) // into / out of the stretcher: prime it at the heard position
        v.primed = false;
    v.stretched = stretched;
    v.rate = rate;

    if (stretched)
    {
        auto& st = *v.stretcher;
        const auto lead = static_cast<double>(st.inputLatency()) + st.outputLatency() * rate;
        if (!v.primed)
        {
            // Prime with what comes before the heard position, so the first output is that position.
            const auto end = static_cast<std::int64_t>(std::llround(v.position + lead));
            const int m = st.seekLength();
            for (int i = 0; i < m; ++i)
            {
                seekL[static_cast<std::size_t>(i)] = frame(v, 0, end - m + i);
                seekR[static_cast<std::size_t>(i)] = frame(v, 1, end - m + i);
            }
            const float* in[] = {seekL.data(), seekR.data()};
            st.reset();
            st.seek(in, m, rate);
            v.feed = end;
            v.primed = true;
        }
        const double positionEnd = v.position + n * rate;
        const auto target = static_cast<std::int64_t>(std::llround(positionEnd + lead));
        const int count = static_cast<int>(std::clamp<std::int64_t>(target - v.feed, 0, static_cast<std::int64_t>(inL.size())));
        for (int i = 0; i < count; ++i)
        {
            inL[static_cast<std::size_t>(i)] = frame(v, 0, v.feed + i);
            inR[static_cast<std::size_t>(i)] = frame(v, 1, v.feed + i);
        }
        v.feed += count;
        const float* in[] = {inL.data(), inR.data()};
        float* out[] = {outL.data(), outR.data()};
        st.process(in, count, out, n);
    }

    const float gain = s.gain;
    for (int i = 0; i < n; ++i)
    {
        float l = 0.0f, r = 0.0f;
        if (stretched)
        {
            l = outL[static_cast<std::size_t>(i)];
            r = outR[static_cast<std::size_t>(i)];
        }
        else
        {
            const auto p = static_cast<std::int64_t>(v.position);
            l = frame(v, 0, p);
            r = frame(v, 1, p);
        }
        float g = gain;
        if (v.fadeIn > 0)
        {
            g *= 1.0f - static_cast<float>(v.fadeIn) / static_cast<float>(fadeSamples);
            --v.fadeIn;
        }
        if (v.fadeOut >= 0)
        {
            g *= static_cast<float>(v.fadeOut) / static_cast<float>(fadeSamples);
            if (--v.fadeOut <= 0)
            {
                v.fadeOut = 0;
                left[start + i] += g * l;
                right[start + i] += g * r;
                v.active = false;
                return;
            }
        }
        left[start + i] += g * l;
        right[start + i] += g * r;
        v.position += rate;
        if (v.position >= len)
        {
            if (!loop)
            {
                v.active = false; // a one-shot / gate at its end
                return;
            }
            v.position -= len * std::floor(v.position / len);
        }
    }
}

} // namespace devildj::engine
