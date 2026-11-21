#include "engine/CueDevice.h"

#include "util/Log.h"
#include "util/RealtimeGuard.h"

#include <algorithm>
#include <cmath>

namespace devildj::engine
{

CueDevice::CueDevice(AudioDeviceService& mainDevice, dsp::DriftBuffer& cueBuffer)
    : main(mainDevice)
    , buffer(cueBuffer)
{
}

CueDevice::~CueDevice()
{
    close();
}

juce::StringArray CueDevice::availableDevices()
{
    auto names = main.getOutputDeviceNames(main.getCurrentDeviceType());
    names.removeString(main.getStatus().deviceName);
    return names;
}

juce::String CueDevice::open(const juce::String& deviceName)
{
    close();
    if (deviceName.isEmpty())
        return "no headphone device chosen";
    if (deviceName == main.getStatus().deviceName)
        return "the headphones need another device than the master";
    juce::AudioIODeviceType* type = nullptr;
    for (auto* t : main.getDeviceManager().getAvailableDeviceTypes())
        if (t->getTypeName() == main.getCurrentDeviceType())
            type = t;
    if (type == nullptr)
        return "no audio driver";
    type->scanForDevices();
    device.reset(type->createDevice(deviceName, {}));
    if (device == nullptr)
        return "can't find " + deviceName;
    const auto mainStatus = main.getStatus();
    juce::BigInteger outputs;
    outputs.setRange(0, 2, true);
    auto error = device->open({}, outputs, mainStatus.sampleRate, mainStatus.bufferSize);
    if (error.isNotEmpty()) // another rate or size: the device's own (the buffer resamples)
        error = device->open({}, outputs, 0.0, 0);
    if (error.isNotEmpty())
    {
        device.reset();
        return error;
    }
    buffer.restart();
    device->start(this);
    util::logInfo("Audio: headphones on {} ({:.0f} Hz, {} samples)", deviceName.toStdString(),
                  device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
    return {};
}

void CueDevice::close()
{
    if (device == nullptr)
        return;
    device->stop();
    device->close();
    device.reset();
    peak = 0.0f;
}

CueDevice::Status CueDevice::status() const
{
    Status s;
    if (device == nullptr)
        return s;
    s.open = device->isOpen();
    s.deviceName = device->getName();
    s.sampleRate = device->getCurrentSampleRate();
    s.bufferSize = device->getCurrentBufferSizeSamples();
    s.delayMs = s.sampleRate > 0.0
                    ? 1000.0 * (buffer.fillFrames() + device->getOutputLatencyInSamples() + s.bufferSize) / s.sampleRate
                    : 0.0;
    s.dropouts = buffer.underruns();
    s.peak = peak.load();
    s.playing = buffer.playing();
    return s;
}

void CueDevice::audioDeviceAboutToStart(juce::AudioIODevice* d)
{
    buffer.setReader(d->getCurrentSampleRate(), d->getCurrentBufferSizeSamples());
    buffer.restart();
}

void CueDevice::audioDeviceStopped() {}

void CueDevice::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int numOutputs,
                                                 int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    util::ScopedRealtimeContext realtime; // the audio rules hold on this thread too
    if (numOutputs <= 0 || outputs[0] == nullptr)
        return;
    float* right = numOutputs > 1 ? outputs[1] : nullptr;
    buffer.read(outputs[0], right, numSamples);
    if (right == nullptr) // a mono device: the left channel only (the cue is mono-compatible)
        right = outputs[0];
    for (int ch = 2; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            std::fill(outputs[ch], outputs[ch] + numSamples, 0.0f);
    float level = peak.load(std::memory_order_relaxed) * 0.9f; // a meter that falls back in ~0.2 s
    for (int i = 0; i < numSamples; ++i)
        level = std::max(level, std::max(std::abs(outputs[0][i]), std::abs(right[i])));
    peak.store(level, std::memory_order_relaxed);
}

} // namespace devildj::engine
