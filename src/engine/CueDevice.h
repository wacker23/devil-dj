#pragma once

#include "dsp/DriftBuffer.h"
#include "engine/AudioDeviceService.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <memory>

namespace devildj::engine
{

// A second output device for the headphone cue (P2-25): the master plays on the main device (AudioDeviceService),
// the cue on this one - e.g. the laptop speakers and its headphone jack when Windows shows them as two devices, or
// the laptop plus a USB headset. The engine writes the cue bus into a dsp::DriftBuffer from its callback; this
// device's own callback (a second audio thread, the driver's) reads it, resampled to follow the two clocks.
// The audio thread rules hold there too: the callback only reads the buffer and a level meter.
//
// Same driver type as the main device. Message thread for everything but the callback.
class CueDevice final : private juce::AudioIODeviceCallback
{
public:
    CueDevice(AudioDeviceService& mainDevice, dsp::DriftBuffer& cueBuffer);
    ~CueDevice() override;

    CueDevice(const CueDevice&) = delete;
    CueDevice& operator=(const CueDevice&) = delete;

    // The main driver's output devices, without the one the master is on.
    juce::StringArray availableDevices();

    // Opens and starts a device (at the master's rate and buffer size when it can). Error, or empty.
    juce::String open(const juce::String& deviceName);
    void close();

    struct Status
    {
        bool open = false;
        juce::String deviceName;
        double sampleRate = 0.0;
        int bufferSize = 0;
        double delayMs = 0.0;       // the buffer's fill + this device's own latency: how far the cue lags
        std::uint32_t dropouts = 0; // the buffer ran dry
        float peak = 0.0f;          // recent output level (linear)
        bool playing = false;       // the buffer is primed and playing
    };
    Status status() const;

private:
    void audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs, float* const* outputs,
                                          int numOutputs, int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    AudioDeviceService& main;
    dsp::DriftBuffer& buffer;
    std::unique_ptr<juce::AudioIODevice> device;
    std::atomic<float> peak{0.0f};
};

} // namespace devildj::engine
