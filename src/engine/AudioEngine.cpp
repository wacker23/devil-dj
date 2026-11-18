#include "engine/AudioEngine.h"

#include "util/RealtimeGuard.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace devildj::engine
{

namespace
{
constexpr double kGainRampSeconds = 0.020;
constexpr float kLoadSmoothing = 0.1f; // one-pole coefficient per block
constexpr float kPeakDecay = 0.995f;   // per block (~4 s to halve at 512 samples / 48 kHz)

template <class... Ts>
struct Overloaded : Ts...
{
    using Ts::operator()...;
};
} // namespace

bool AudioEngine::send(const EngineCommand& command) noexcept
{
    if (commands.tryPush(command))
        return true;
    dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void AudioEngine::prepare(double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    capacity = std::max(maxBlockSize, 1);
    masterBus.setSize(2, capacity, false, true, false);
    cueBus.setSize(2, capacity, false, true, false);
    tone.gain.reset(sampleRate, kGainRampSeconds);
    reset();
}

void AudioEngine::reset() noexcept
{
    tone.phase = 0.0;
    tone.phaseIncrement = 2.0 * std::numbers::pi * tone.frequencyHz / sampleRate;
    tone.gain.setCurrentAndTargetValue(tone.on ? tone.gain.getTargetValue() : 0.0f);
    masterBus.clear();
    cueBus.clear();

    const auto processed = current.commandsProcessed;
    current = EngineState{};
    current.sampleRate = sampleRate;
    current.commandsProcessed = processed;
    current.outputLayout = layout;
    current.testToneOn = tone.on;
    current.testToneHz = tone.frequencyHz;
    current.testToneBus = tone.bus;
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    prepare(device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
}

void AudioEngine::audioDeviceStopped() {}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* /*inputs*/, int /*numInputs*/,
                                                   float* const* outputs, int numOutputs, int numSamples,
                                                   const juce::AudioIODeviceCallbackContext& /*context*/)
{
    process(outputs, numOutputs, numSamples);
}

void AudioEngine::process(float* const* outputs, int numChannels, int numSamples) noexcept
{
    util::ScopedRealtimeContext realtime; // Debug: any allocation from here on is reported
    const auto startTicks = juce::Time::getHighResolutionTicks();

    drainCommands();

    if (capacity == 0) // not prepared: stay silent
    {
        for (int ch = 0; ch < numChannels; ++ch)
            if (outputs[ch] != nullptr)
                juce::FloatVectorOperations::clear(outputs[ch], numSamples);
    }
    else
    {
        for (int offset = 0; offset < numSamples; offset += capacity)
            renderChunk(outputs, numChannels, offset, std::min(capacity, numSamples - offset));
    }

    const auto elapsed = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - startTicks);
    publish(numChannels, numSamples, elapsed);
}

void AudioEngine::renderChunk(float* const* outputs, int numChannels, int offset, int numSamples) noexcept
{
    masterBus.clear(0, numSamples);
    cueBus.clear(0, numSamples);

    renderTestTone(numSamples);

    // Point at this chunk of each device output channel.
    float* chunk[kMaxOutputChannels] = {};
    const int channels = std::min(numChannels, kMaxOutputChannels);
    for (int ch = 0; ch < channels; ++ch)
        chunk[ch] = outputs[ch] != nullptr ? outputs[ch] + offset : nullptr;
    for (int ch = channels; ch < numChannels; ++ch) // more outputs than we track: keep them silent
        if (outputs[ch] != nullptr)
            juce::FloatVectorOperations::clear(outputs[ch] + offset, numSamples);

    routeOutputs(layout, masterBus.getReadPointer(0), masterBus.getReadPointer(1), cueBus.getReadPointer(0),
                 cueBus.getReadPointer(1), chunk, channels, numSamples);
}

void AudioEngine::drainCommands() noexcept
{
    EngineCommand command;
    for (int i = 0; i < kMaxCommandsPerBlock && commands.tryPop(command); ++i)
    {
        std::visit(
            Overloaded{[this](const SetTestTone& c) { apply(c); }, [this](const SetOutputLayout& c) { apply(c); }},
            command);
        ++current.commandsProcessed;
    }
}

void AudioEngine::apply(const SetTestTone& command) noexcept
{
    const float hz = std::clamp(command.frequencyHz, 20.0f, static_cast<float>(sampleRate * 0.45));
    tone.frequencyHz = hz;
    tone.phaseIncrement = 2.0 * std::numbers::pi * hz / sampleRate;

    const bool busChanged = command.bus != tone.bus;
    if (command.on && (!tone.on || busChanged))
    {
        tone.phase = 0.0; // start at a zero crossing; the gain ramp handles the rest
        if (busChanged)
            tone.gain.setCurrentAndTargetValue(0.0f); // fade in on the new bus instead of jumping
    }
    tone.on = command.on;
    tone.bus = command.bus;

    const double angle = (std::clamp(command.pan, -1.0f, 1.0f) + 1.0) * std::numbers::pi / 4.0;
    tone.gainLeft = static_cast<float>(std::cos(angle));
    tone.gainRight = static_cast<float>(std::sin(angle));
    tone.gain.setTargetValue(command.on ? juce::Decibels::decibelsToGain(command.gainDb) : 0.0f);

    current.testToneOn = tone.on;
    current.testToneHz = hz;
    current.testToneBus = tone.bus;
}

void AudioEngine::apply(const SetOutputLayout& command) noexcept
{
    layout = command.layout;
    current.outputLayout = layout;
}

void AudioEngine::renderTestTone(int numSamples) noexcept
{
    if (!tone.on && !tone.gain.isSmoothing() && tone.gain.getCurrentValue() == 0.0f)
        return; // silent: nothing to add

    auto& bus = tone.bus == Bus::Cue ? cueBus : masterBus;
    float* left = bus.getWritePointer(0);
    float* right = bus.getWritePointer(1);
    for (int i = 0; i < numSamples; ++i)
    {
        const float sample = static_cast<float>(std::sin(tone.phase)) * tone.gain.getNextValue();
        tone.phase += tone.phaseIncrement;
        if (tone.phase >= 2.0 * std::numbers::pi)
            tone.phase -= 2.0 * std::numbers::pi;
        left[i] += sample * tone.gainLeft;
        right[i] += sample * tone.gainRight;
    }
}

void AudioEngine::publish(int numChannels, int numSamples, double elapsedSeconds) noexcept
{
    const double budget = numSamples / sampleRate;
    const auto load = budget > 0.0 ? static_cast<float>(elapsedSeconds / budget) : 0.0f;

    ++current.blockCounter;
    current.blockSize = numSamples;
    current.sampleRate = sampleRate;
    current.hostTimeSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
    current.cpuLoad += kLoadSmoothing * (load - current.cpuLoad);
    current.cpuLoadPeak = std::max(load, current.cpuLoadPeak * kPeakDecay);
    if (load > 1.0f)
        ++current.xruns;
    current.outputChannels = numChannels;
    current.cueRouted = cueIsRouted(layout, numChannels);

    state.write(current);
}

} // namespace devildj::engine
