#include "engine/Voices.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numbers>

namespace devildj::engine
{

namespace
{
constexpr float kStealFadeSeconds = 0.002f;
constexpr float kChokeFadeSeconds = 0.005f;
constexpr float kMinRampSeconds = 0.0005f; // even a zero attack / release ramps over half a millisecond

float stepFor(double seconds, double rate)
{
    return static_cast<float>(1.0 / std::max(1.0, std::max(seconds, static_cast<double>(kMinRampSeconds)) * rate));
}
} // namespace

void Voices::prepare(double sampleRate, int) noexcept
{
    rate = sampleRate;
    reset();
}

void Voices::reset() noexcept
{
    for (auto* pool : {&samples, &sampleTails})
        for (auto& v : *pool)
            v.stage = Stage::Idle;
    for (auto* pool : {&synths, &synthTails})
        for (auto& v : *pool)
            v.stage = Stage::Idle;
    for (auto* pool : {&drums, &drumTails})
        for (auto& v : *pool)
            v.stage = Stage::Idle;
}

void Voices::setSynthSound(std::uint16_t index, const dsp::SynthParams& sound) noexcept
{
    if (index < kSounds)
    {
        synthSounds[index] = sound;
        synthSet[index] = true;
    }
}

void Voices::setDrumSound(std::uint16_t index, const DrumSound& sound) noexcept
{
    if (index < kSounds)
        drumSounds[index] = sound;
}

void Voices::setSample(std::uint16_t index, const SampleData* data) noexcept
{
    if (index < kSounds)
        sounds[index] = data;
}

Voices::Voice& Voices::allocate(std::span<Voice> pool, std::span<Voice> tails) noexcept
{
    for (auto& v : pool)
        if (v.stage == Stage::Idle)
            return v;
    // Full: the oldest releasing (or fading) voice, else the oldest playing one.
    Voice* pick = nullptr;
    for (auto& v : pool)
        if ((v.stage == Stage::Release || v.stage == Stage::Fade) && (pick == nullptr || v.serial < pick->serial))
            pick = &v;
    if (pick == nullptr)
        for (auto& v : pool)
            if (pick == nullptr || v.serial < pick->serial)
                pick = &v;
    ++steals;
    // Its sound goes on in a tail slot, fading out (a tail slot still busy is simply replaced).
    auto& tail = tails[static_cast<std::size_t>(pick - pool.data())];
    tail = *pick;
    if (tail.delay > 0)
        tail.stage = Stage::Idle; // never started: nothing to fade
    else if (tail.stage != Stage::Fade)
    {
        tail.stage = Stage::Fade;
        tail.fadeStep = std::max(tail.level, 1e-6f) * stepFor(kStealFadeSeconds, rate);
    }
    tail.chokeAt = -1;
    return *pick;
}

void Voices::setUp(Voice& v, const ScheduledEvent& e, int offset) noexcept
{
    v = Voice{};
    v.kind = e.kind;
    v.source = e.source;
    v.chokeGroup = e.chokeGroup;
    v.serial = ++serialCounter;
    v.delay = std::max(0, offset);
    v.stage = Stage::Attack;
    v.attackStep = stepFor(e.attack, rate);
    v.decayStep = stepFor(std::max(0.001f, e.decay), rate);
    v.sustain = std::clamp(e.sustain, 0.0f, 1.0f);
    v.releaseStep = stepFor(e.release, rate);
    v.gateSamples = e.durationSeconds > 0.0f ? static_cast<std::int64_t>(std::llround(e.durationSeconds * rate)) : -1;
    const float pan = std::clamp(e.pan, 0.0f, 1.0f) * std::numbers::pi_v<float> * 0.5f;
    const float gain = std::max(0.0f, e.gain) * std::clamp(e.velocity, 0.0f, 1.0f);
    v.gainL = gain * std::cos(pan);
    v.gainR = gain * std::sin(pan);
    v.delaySend = std::clamp(e.delaySend, 0.0f, 1.0f);
    v.roomSend = std::clamp(e.roomSend, 0.0f, 1.0f);
    v.lowpass = e.cutoffHz > 0.0f && e.cutoffHz < 19999.0f;
    v.highpass = e.hpfHz > 10.0f;
    const double q = 0.7071 + 9.0 * std::clamp(static_cast<double>(e.resonance), 0.0, 1.0);
    if (v.lowpass)
        v.lp = dsp::SvfCoeffs::make(e.cutoffHz, rate, q);
    if (v.highpass)
        v.hp = dsp::SvfCoeffs::make(e.hpfHz, rate, 0.7071);
    const double pitch = std::pow(2.0, (static_cast<double>(e.note) - 60.0) / 12.0);
    if (e.kind == VoiceKind::Sample)
    {
        v.sample = sounds[e.soundIndex % kSounds];
        if (v.sample != nullptr)
            v.increment = std::max(0.0, static_cast<double>(e.speed)) * pitch * v.sample->sampleRate / rate;
    }
    else if (e.kind == VoiceKind::DrumSynth)
    {
        const auto& sound = drumSounds[e.soundIndex % kSounds];
        v.gateSamples = -1; // one-shot
        v.sustain = 1.0f;
        v.drum.prepare(rate);
        v.drum.trigger(sound.kind, sound.params, 1.0f, static_cast<std::uint32_t>(v.serial));
    }
    else if (synthSet[e.soundIndex % kSounds])
    {
        const auto i = static_cast<std::size_t>(e.soundIndex % kSounds);
        auto& last = lastNote[static_cast<std::size_t>(std::min(static_cast<int>(e.source), kSources - 1))][i];
        v.synthEngine = true;
        v.synthGate = v.gateSamples;
        v.gateSamples = -1; // the synth's own envelopes
        v.sustain = 1.0f;
        v.synth.prepare(rate);
        v.synth.noteOn(synthSounds[i], e.note, 1.0f, static_cast<std::uint32_t>(v.serial), last > 0.0f ? last : -1.0f);
        last = e.note;
    }
    else
    {
        const double hz = 440.0 * std::pow(2.0, (static_cast<double>(e.note) - 69.0) / 12.0);
        v.phaseIncrement = hz / rate;
    }
}

void Voices::choke(SourceId source, std::uint8_t group, int offset, const Voice* except) noexcept
{
    const auto apply = [&](Voice& v)
    {
        if (&v != except && v.stage != Stage::Idle && v.source == source && v.chokeGroup == group)
        {
            if (v.delay >= offset && v.delay > 0)
                v.stage = Stage::Idle; // would only start after the new one: never mind
            else
                v.chokeAt = offset;
        }
    };
    for (auto& v : samples)
        apply(v);
    for (auto& v : synths)
        apply(v);
    for (auto& v : drums)
        apply(v);
}

std::uint64_t Voices::start(const ScheduledEvent& e, int offset) noexcept
{
    Voice* v = nullptr;
    if (e.kind == VoiceKind::Sample)
    {
        if (sounds[e.soundIndex % kSounds] == nullptr)
        {
            ++unhandledEvents;
            return 0;
        }
        v = &allocate(samples, sampleTails);
    }
    else if (e.kind == VoiceKind::Synth)
        v = &allocate(synths, synthTails);
    else
    {
        const auto s = static_cast<std::size_t>(std::min(static_cast<int>(e.source), kSources - 1)) * kDrumVoicesPerSource;
        v = &allocate(std::span<Voice>(drums).subspan(s, kDrumVoicesPerSource),
                      std::span<Voice>(drumTails).subspan(s, kDrumVoicesPerSource));
    }
    setUp(*v, e, offset);
    if (e.chokeGroup != 0)
        choke(e.source, e.chokeGroup, std::max(0, offset), v);
    return v->serial;
}

bool Voices::playing(std::uint64_t id) const noexcept
{
    const auto in = [&](const auto& pool)
    { return std::any_of(pool.begin(), pool.end(), [&](const Voice& v) { return v.serial == id && v.stage != Stage::Idle && v.stage != Stage::Fade; }); };
    return id != 0 && (in(samples) || in(synths) || in(drums));
}

bool Voices::envelopeStep(Voice& v) noexcept
{
    switch (v.stage)
    {
        case Stage::Attack:
            v.level += v.attackStep;
            if (v.level >= 1.0f)
            {
                v.level = 1.0f;
                v.stage = Stage::Decay;
            }
            break;
        case Stage::Decay:
            v.level -= v.decayStep;
            if (v.level <= v.sustain)
            {
                v.level = v.sustain;
                v.stage = Stage::Sustain;
            }
            break;
        case Stage::Sustain:
            break;
        case Stage::Release:
            v.level -= v.releaseStep;
            if (v.level <= 0.0f)
                return false;
            break;
        case Stage::Fade:
            v.level -= v.fadeStep;
            if (v.level <= 0.0f)
                return false;
            break;
        case Stage::Idle:
            return false;
    }
    if (v.gateSamples >= 0 && --v.gateSamples < 0 && v.stage != Stage::Fade && v.stage != Stage::Release)
        v.stage = Stage::Release;
    return true;
}

void Voices::render(Voice& v, const VoiceBuses& b, int numSamples) noexcept
{
    int i = std::min(v.delay, numSamples);
    v.delay -= i;
    if (v.delay > 0)
    {
        if (v.chokeAt >= 0)
            v.stage = Stage::Idle;
        return;
    }
    for (; i < numSamples; ++i)
    {
        if (i == v.chokeAt && v.stage != Stage::Fade)
        {
            v.stage = Stage::Fade;
            v.fadeStep = std::max(v.level, 1e-6f) * stepFor(kChokeFadeSeconds, rate);
        }
        // The source.
        float l = 0.0f, r = 0.0f;
        if (v.kind == VoiceKind::Sample)
        {
            const auto* s = v.sample;
            const auto at = static_cast<int>(v.position);
            if (s == nullptr || at + 1 >= s->numFrames)
            {
                v.stage = Stage::Idle;
                break;
            }
            const float t = static_cast<float>(v.position - at);
            const float* c0 = s->channels[0];
            l = c0[at] + (c0[at + 1] - c0[at]) * t;
            if (s->numChannels > 1)
            {
                const float* c1 = s->channels[1];
                r = c1[at] + (c1[at + 1] - c1[at]) * t;
            }
            else
                r = l;
            v.position += v.increment;
        }
        else if (v.kind == VoiceKind::DrumSynth)
        {
            l = r = v.drum.next();
            if (v.drum.finished())
            {
                v.stage = Stage::Idle;
                break;
            }
        }
        else if (v.synthEngine)
        {
            if (v.synthGate >= 0 && v.synthGate-- == 0)
                v.synth.noteOff();
            l = r = v.synth.next();
            if (v.synth.finished())
            {
                v.stage = Stage::Idle;
                break;
            }
        }
        else
        {
            l = r = static_cast<float>(std::sin(2.0 * std::numbers::pi * v.phase));
            v.phase += v.phaseIncrement;
            if (v.phase >= 1.0)
                v.phase -= 1.0;
        }
        if (!envelopeStep(v))
        {
            v.stage = Stage::Idle;
            break;
        }
        // Filter, envelope, pan, out.
        double fl = l, fr = r;
        if (v.lowpass)
        {
            fl = v.lpState[0].process(v.lp, fl).low;
            fr = v.lpState[1].process(v.lp, fr).low;
        }
        if (v.highpass)
        {
            fl = v.hpState[0].process(v.hp, fl).high;
            fr = v.hpState[1].process(v.hp, fr).high;
        }
        const float outL = static_cast<float>(fl) * v.level * v.gainL;
        const float outR = static_cast<float>(fr) * v.level * v.gainR;
        b.left[i] += outL;
        b.right[i] += outR;
        if (v.delaySend > 0.0f && b.delayLeft != nullptr)
        {
            b.delayLeft[i] += outL * v.delaySend;
            b.delayRight[i] += outR * v.delaySend;
        }
        if (v.roomSend > 0.0f && b.reverbLeft != nullptr)
        {
            b.reverbLeft[i] += outL * v.roomSend;
            b.reverbRight[i] += outR * v.roomSend;
        }
    }
    v.chokeAt = -1;
}

void Voices::process(const VoiceBuses& buses, int numSamples) noexcept
{
    for (auto* pool : {&samples, &sampleTails})
        for (auto& v : *pool)
            if (v.stage != Stage::Idle)
                render(v, buses, numSamples);
    for (auto* pool : {&synths, &synthTails})
        for (auto& v : *pool)
            if (v.stage != Stage::Idle)
                render(v, buses, numSamples);
    for (auto* pool : {&drums, &drumTails})
        for (auto& v : *pool)
            if (v.stage != Stage::Idle)
                render(v, buses, numSamples);
}

void Voices::releaseAll() noexcept
{
    const auto fade = [&](auto& pool)
    {
        for (auto& v : pool)
            if (v.stage != Stage::Idle)
            {
                if (v.delay > 0)
                    v.stage = Stage::Idle;
                else if (v.stage != Stage::Fade)
                {
                    v.stage = Stage::Fade;
                    v.fadeStep = std::max(v.level, 1e-6f) * stepFor(kStealFadeSeconds, rate);
                }
            }
    };
    fade(samples);
    fade(sampleTails);
    fade(synths);
    fade(synthTails);
    fade(drums);
    fade(drumTails);
}

int Voices::activeDrumVoices() const noexcept
{
    return static_cast<int>(std::count_if(drums.begin(), drums.end(), [](const Voice& v) { return v.stage != Stage::Idle; }));
}

int Voices::activeSampleVoices() const noexcept
{
    return static_cast<int>(std::count_if(samples.begin(), samples.end(), [](const Voice& v) { return v.stage != Stage::Idle; }));
}

int Voices::activeSynthVoices() const noexcept
{
    return static_cast<int>(std::count_if(synths.begin(), synths.end(), [](const Voice& v) { return v.stage != Stage::Idle; }));
}

} // namespace devildj::engine
