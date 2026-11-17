#include "engine/AudioDeviceService.h"

#include "util/Log.h"

namespace devildj::engine
{

// Built with String concatenation, not String::formatted: on Windows formatted() uses wide printf, where
// %s doesn't take UTF-8 and device names come out garbled.
juce::String describeStatus(const AudioDeviceStatus& s)
{
    if (!s.isOpen)
        return "no audio device open" + (s.lastError.isEmpty() ? juce::String() : " (" + s.lastError + ")");

    return s.typeName + " / " + s.deviceName + ", " + juce::String(s.sampleRate, 0) + " Hz, " +
           juce::String(s.bufferSize) + " samples (" + juce::String(s.bufferMs, 1) + " ms), output latency " +
           juce::String(s.totalOutputLatencyMs, 1) + " ms";
}

AudioDeviceService::AudioDeviceService()
{
    deviceManager.addChangeListener(this);
}

AudioDeviceService::~AudioDeviceService()
{
    deviceManager.removeChangeListener(this);
    setAudioCallback(nullptr);
    deviceManager.closeAudioDevice();
}

void AudioDeviceService::addDeviceType(std::unique_ptr<juce::AudioIODeviceType> type)
{
    customTypesAdded = true;
    deviceManager.addAudioDeviceType(std::move(type));
}

juce::String AudioDeviceService::initialise(const juce::String& savedStateXml)
{
    std::unique_ptr<juce::XmlElement> state;
    if (savedStateXml.isNotEmpty())
    {
        state = juce::parseXML(savedStateXml);
        if (state == nullptr)
            util::logWarning("Audio: ignoring unreadable saved device settings");
    }

    // No inputs yet (the mic channel comes in P7-03). Falls back to the default device if the saved one
    // is missing (e.g. a USB interface that isn't plugged in).
    auto error = deviceManager.initialise(0, kNumOutputChannels, state.get(), true);
    if (error.isNotEmpty())
        util::logError("Audio: {}", error.toStdString());

    lastLoggedDescription = describeStatus(getStatus());
    util::logInfo("Audio: {}", lastLoggedDescription.toStdString());
    return error;
}

juce::String AudioDeviceService::getStateXml() const
{
    if (auto xml = deviceManager.createStateXml())
        return xml->toString(juce::XmlElement::TextFormat().singleLine().withoutHeader());
    return {};
}

juce::StringArray AudioDeviceService::getDeviceTypeNames()
{
    juce::StringArray names;
    for (auto* type : deviceManager.getAvailableDeviceTypes())
        names.add(type->getTypeName());
    return names;
}

juce::String AudioDeviceService::getCurrentDeviceType() const
{
    return deviceManager.getCurrentAudioDeviceType();
}

juce::StringArray AudioDeviceService::getOutputDeviceNames(const juce::String& typeName)
{
    for (auto* type : deviceManager.getAvailableDeviceTypes())
        if (type->getTypeName() == typeName)
        {
            type->scanForDevices();
            return type->getDeviceNames(false);
        }
    return {};
}

juce::String AudioDeviceService::setDeviceType(const juce::String& typeName)
{
    if (!getDeviceTypeNames().contains(typeName))
        return "Unknown audio device type: " + typeName;

    deviceManager.setCurrentAudioDeviceType(typeName, true);
    if (deviceManager.getCurrentAudioDevice() == nullptr)
        return "Couldn't open a device of type " + typeName;
    return {};
}

juce::String AudioDeviceService::setOutputDevice(const juce::String& deviceName)
{
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.outputDeviceName = deviceName;
    setup.inputDeviceName = {};
    setup.useDefaultOutputChannels = true;
    return applySetup(setup);
}

juce::String AudioDeviceService::setSampleRate(double sampleRate)
{
    if (!getAvailableSampleRates().contains(sampleRate))
        return juce::String::formatted("Sample rate %.0f Hz isn't supported by this device", sampleRate);

    auto setup = deviceManager.getAudioDeviceSetup();
    setup.sampleRate = sampleRate;
    return applySetup(setup);
}

juce::String AudioDeviceService::setBufferSize(int samples)
{
    if (!getAvailableBufferSizes().contains(samples))
        return juce::String::formatted("Buffer size %d isn't supported by this device", samples);

    auto setup = deviceManager.getAudioDeviceSetup();
    setup.bufferSize = samples;
    return applySetup(setup);
}

juce::String AudioDeviceService::applySetup(const juce::AudioDeviceManager::AudioDeviceSetup& setup)
{
    auto error = deviceManager.setAudioDeviceSetup(setup, true);
    if (error.isNotEmpty())
        util::logWarning("Audio: {}", error.toStdString());
    return error;
}

juce::Array<double> AudioDeviceService::getAvailableSampleRates() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getAvailableSampleRates();
    return {};
}

juce::Array<int> AudioDeviceService::getAvailableBufferSizes() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
        return device->getAvailableBufferSizes();
    return {};
}

AudioDeviceStatus AudioDeviceService::getStatus() const
{
    AudioDeviceStatus s;
    s.typeName = deviceManager.getCurrentAudioDeviceType();

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr || !device->isOpen())
    {
        s.lastError = device != nullptr ? device->getLastError() : juce::String();
        return s;
    }

    s.isOpen = true;
    s.deviceName = device->getName();
    s.sampleRate = device->getCurrentSampleRate();
    s.bufferSize = device->getCurrentBufferSizeSamples();
    s.outputLatencySamples = device->getOutputLatencyInSamples();
    if (s.sampleRate > 0.0)
    {
        s.bufferMs = 1000.0 * s.bufferSize / s.sampleRate;
        s.outputLatencyMs = 1000.0 * s.outputLatencySamples / s.sampleRate;
        s.totalOutputLatencyMs = s.bufferMs + s.outputLatencyMs;
    }
    s.xruns = deviceManager.getXRunCount();
    s.cpuLoad = deviceManager.getCpuUsage();
    s.lastError = device->getLastError();
    return s;
}

void AudioDeviceService::setAudioCallback(juce::AudioIODeviceCallback* callback)
{
    if (callback == currentCallback)
        return;
    if (currentCallback != nullptr)
        deviceManager.removeAudioCallback(currentCallback);
    currentCallback = callback;
    if (currentCallback != nullptr)
        deviceManager.addAudioCallback(currentCallback);
}

void AudioDeviceService::changeListenerCallback(juce::ChangeBroadcaster*)
{
    const auto description = describeStatus(getStatus());
    if (description != lastLoggedDescription)
    {
        lastLoggedDescription = description;
        util::logInfo("Audio: {}", description.toStdString());
    }

    if (onDeviceChanged)
        onDeviceChanged();
}

} // namespace devildj::engine
