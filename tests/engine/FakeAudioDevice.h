#pragma once

// A fake audio device type for tests: no hardware, a thread calls the audio callback once per buffer period.

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace devildj::test
{

class FakeAudioDevice final : public juce::AudioIODevice
{
public:
    FakeAudioDevice(const juce::String& deviceName, const juce::String& typeName, int outputChannels = 2)
        : AudioIODevice(deviceName, typeName)
        , numOutputs(outputChannels)
    {
    }

    ~FakeAudioDevice() override { close(); }

    juce::StringArray getOutputChannelNames() override
    {
        juce::StringArray names;
        for (int i = 0; i < numOutputs; ++i)
            names.add("Out " + juce::String(i + 1));
        return names;
    }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return {44100.0, 48000.0}; }
    juce::Array<int> getAvailableBufferSizes() override { return {128, 256, 512}; }
    int getDefaultBufferSize() override { return 256; }

    juce::String open(const juce::BigInteger& /*inputs*/, const juce::BigInteger& outputs, double rate,
                      int bufferSize) override
    {
        sampleRate = rate > 0 ? rate : 48000.0;
        bufferSamples = bufferSize > 0 ? bufferSize : getDefaultBufferSize();
        activeOutputs = outputs;
        opened = true;
        return {};
    }

    void close() override
    {
        stop();
        opened = false;
    }

    bool isOpen() override { return opened; }

    void start(juce::AudioIODeviceCallback* cb) override
    {
        if (!opened || cb == nullptr || running)
            return;
        callback = cb;
        callback->audioDeviceAboutToStart(this);
        running = true;
        thread = std::thread([this] { run(); });
    }

    void stop() override
    {
        if (!running)
            return;
        running = false;
        thread.join();
        if (callback != nullptr)
            callback->audioDeviceStopped();
        callback = nullptr;
    }

    bool isPlaying() override { return running; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return bufferSamples; }
    double getCurrentSampleRate() override { return sampleRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return activeOutputs; }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 64; }
    int getInputLatencyInSamples() override { return 0; }

    std::atomic<int> callbacks{0};

private:
    void run()
    {
        const int channels = juce::jmax(1, activeOutputs.countNumberOfSetBits());
        juce::AudioBuffer<float> buffer(channels, bufferSamples);
        const auto period = std::chrono::duration<double>(bufferSamples / sampleRate);
        auto next = std::chrono::steady_clock::now();
        const juce::AudioIODeviceCallbackContext context{};
        while (running)
        {
            buffer.clear();
            callback->audioDeviceIOCallbackWithContext(nullptr, 0, buffer.getArrayOfWritePointers(), channels,
                                                       bufferSamples, context);
            ++callbacks;
            next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
            std::this_thread::sleep_until(next);
        }
    }

    int numOutputs = 2;
    double sampleRate = 48000.0;
    int bufferSamples = 256;
    juce::BigInteger activeOutputs;
    bool opened = false;
    std::atomic<bool> running{false};
    juce::AudioIODeviceCallback* callback = nullptr;
    std::thread thread;
};

class FakeAudioDeviceType final : public juce::AudioIODeviceType
{
public:
    FakeAudioDeviceType(const juce::String& typeName, juce::StringArray deviceNames, int outputChannels = 2)
        : AudioIODeviceType(typeName)
        , names(std::move(deviceNames))
        , numOutputs(outputChannels)
    {
    }

    void scanForDevices() override {}
    juce::StringArray getDeviceNames(bool wantInputNames) const override
    {
        return wantInputNames ? juce::StringArray() : names;
    }
    int getDefaultDeviceIndex(bool /*forInput*/) const override { return 0; }
    int getIndexOfDevice(juce::AudioIODevice* device, bool asInput) const override
    {
        return (device == nullptr || asInput) ? -1 : names.indexOf(device->getName());
    }
    bool hasSeparateInputsAndOutputs() const override { return false; }

    juce::AudioIODevice* createDevice(const juce::String& outputName, const juce::String& inputName) override
    {
        const auto name = outputName.isNotEmpty() ? outputName : inputName;
        if (!names.contains(name))
            return nullptr;
        return new FakeAudioDevice(name, getTypeName(), numOutputs); // JUCE takes ownership
    }

private:
    juce::StringArray names;
    int numOutputs;
};

} // namespace devildj::test
