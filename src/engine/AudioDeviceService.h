#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <functional>
#include <memory>

namespace devildj::engine
{

// Snapshot of the open audio device, for settings pages, the debug overlay and logs.
struct AudioDeviceStatus
{
    bool isOpen = false;
    juce::String typeName; // "Windows Audio", "Windows Audio (Exclusive Mode)", "Windows Audio (Low Latency Mode)",
                           // "DirectSound"
    juce::String deviceName;
    double sampleRate = 0.0;
    int bufferSize = 0;                // samples per callback
    int outputLatencySamples = 0;      // as reported by the driver
    double bufferMs = 0.0;             // bufferSize / sampleRate
    double outputLatencyMs = 0.0;      // driver-reported output latency
    double totalOutputLatencyMs = 0.0; // buffer + driver latency: roughly key press -> sound
    int xruns = 0;        // under/overruns since the device started (driver count, or callbacks over budget)
    double cpuLoad = 0.0; // 0..1 share of the buffer time spent in the audio callback
    juce::String lastError;
};

// One-line description for logs, e.g. "Windows Audio / Speakers (Realtek(R) Audio), 48000 Hz, 512 samples ...".
juce::String describeStatus(const AudioDeviceStatus& status);

// Owns the juce::AudioDeviceManager (P1-01): device type, device, sample rate and buffer size; saving and
// restoring the choice as XML; xrun and latency reporting. Message thread only.
//
// The audio engine (P1-02) plugs in with setAudioCallback(); until then the device plays silence.
class AudioDeviceService : private juce::ChangeListener
{
public:
    static constexpr int kNumOutputChannels = 2; // stereo for now; 4-channel cue layouts come in P1-03

    AudioDeviceService();
    ~AudioDeviceService() override;

    AudioDeviceService(const AudioDeviceService&) = delete;
    AudioDeviceService& operator=(const AudioDeviceService&) = delete;

    // Tests: register device types before initialise(). If any type is added, the platform's default
    // types (Windows Audio, DirectSound, ...) are not created.
    void addDeviceType(std::unique_ptr<juce::AudioIODeviceType> type);

    // Opens the device described by savedStateXml (from getStateXml()), or the default device if that's
    // empty or no longer available. Returns an error message, or empty on success.
    juce::String initialise(const juce::String& savedStateXml);

    // The user's explicit choice as XML, or empty if they never changed the default.
    juce::String getStateXml() const;

    juce::StringArray getDeviceTypeNames();
    juce::String getCurrentDeviceType() const;
    juce::StringArray getOutputDeviceNames(const juce::String& typeName);

    // Each returns an error message, or empty on success. The change applies immediately (no restart).
    juce::String setDeviceType(const juce::String& typeName);
    juce::String setOutputDevice(const juce::String& deviceName);
    juce::String setSampleRate(double sampleRate);
    juce::String setBufferSize(int samples);

    // Opens the first 'count' outputs of the current device (2 for stereo/split, 4 for 4-channel cue).
    // Devices with fewer outputs open as many as they have.
    juce::String setOutputChannelCount(int count);
    int getActiveOutputChannelCount() const;
    int getMaxOutputChannelCount() const;

    juce::Array<double> getAvailableSampleRates() const;
    juce::Array<int> getAvailableBufferSizes() const;

    AudioDeviceStatus getStatus() const;

    // The engine's callback; nullptr = silence. Replaces any previous callback.
    void setAudioCallback(juce::AudioIODeviceCallback* callback);

    // Called on the message thread after any device change (type, device, rate, buffer, device lost).
    std::function<void()> onDeviceChanged;

    // For JUCE's device selector component.
    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    juce::String applySetup(const juce::AudioDeviceManager::AudioDeviceSetup& setup);

    juce::AudioDeviceManager deviceManager;
    juce::AudioIODeviceCallback* currentCallback = nullptr;
    bool customTypesAdded = false;
    juce::String lastLoggedDescription;
};

} // namespace devildj::engine
