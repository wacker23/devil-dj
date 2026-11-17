#include "engine/AudioDeviceService.h"

#include "FakeAudioDevice.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using devildj::engine::AudioDeviceService;
using devildj::test::FakeAudioDeviceType;

namespace
{
// Two fake driver types with two devices each, standing in for "Windows Audio", "DirectSound", ...
std::unique_ptr<AudioDeviceService> makeService()
{
    auto service = std::make_unique<AudioDeviceService>();
    service->addDeviceType(
        std::make_unique<FakeAudioDeviceType>("Fake Shared", juce::StringArray{"Speakers", "Headphones"}));
    service->addDeviceType(
        std::make_unique<FakeAudioDeviceType>("Fake Exclusive", juce::StringArray{"USB DAC", "HDMI"}));
    return service;
}

// Counts callbacks; optionally takes longer than the buffer period to provoke xruns.
struct TestCallback final : juce::AudioIODeviceCallback
{
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int numOutputs,
                                          int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                juce::FloatVectorOperations::clear(outputs[ch], numSamples);
        if (slow)
            std::this_thread::sleep_for(std::chrono::milliseconds(12)); // > 256/48000 = 5.3 ms
        ++calls;
    }
    void audioDeviceAboutToStart(juce::AudioIODevice*) override { ++starts; }
    void audioDeviceStopped() override {}

    std::atomic<int> calls{0};
    std::atomic<int> starts{0};
    std::atomic<bool> slow{false};
};

bool waitFor(const std::function<bool()>& condition, int timeoutMs = 3000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!condition())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}
} // namespace

TEST_CASE("AudioDeviceService opens the default device", "[engine][AudioDeviceService]")
{
    juce::ScopedJuceInitialiser_GUI juce;
    auto service = makeService();
    CHECK(service->initialise({}).isEmpty());

    const auto s = service->getStatus();
    CHECK(s.isOpen);
    CHECK(s.typeName == "Fake Shared");
    CHECK(s.deviceName == "Speakers");
    CHECK(s.bufferSize == 256);
    CHECK(s.sampleRate > 0.0);
    CHECK(service->getDeviceTypeNames() == juce::StringArray{"Fake Shared", "Fake Exclusive"});
    CHECK(service->getOutputDeviceNames("Fake Exclusive") == juce::StringArray{"USB DAC", "HDMI"});
    CHECK(service->getStateXml().isEmpty()); // nothing chosen explicitly yet
}

TEST_CASE("AudioDeviceService switches type, device, rate and buffer without restarting",
          "[engine][AudioDeviceService]")
{
    juce::ScopedJuceInitialiser_GUI juce;
    auto service = makeService();
    REQUIRE(service->initialise({}).isEmpty());
    TestCallback callback;
    service->setAudioCallback(&callback);
    REQUIRE(waitFor([&] { return callback.calls > 5; }));

    CHECK(service->setDeviceType("Fake Exclusive").isEmpty());
    CHECK(service->getStatus().typeName == "Fake Exclusive");
    CHECK(service->getStatus().deviceName == "USB DAC");

    CHECK(service->setOutputDevice("HDMI").isEmpty());
    CHECK(service->getStatus().deviceName == "HDMI");

    CHECK(service->setSampleRate(44100.0).isEmpty());
    CHECK(service->getStatus().sampleRate == 44100.0);

    CHECK(service->setBufferSize(128).isEmpty());
    CHECK(service->getStatus().bufferSize == 128);

    // Audio keeps flowing to the same callback after every change.
    const int before = callback.calls;
    CHECK(waitFor([&] { return callback.calls > before + 5; }));
    CHECK(callback.starts >= 4);

    service->setAudioCallback(nullptr);
}

TEST_CASE("AudioDeviceService rejects unsupported choices", "[engine][AudioDeviceService]")
{
    juce::ScopedJuceInitialiser_GUI juce;
    auto service = makeService();
    REQUIRE(service->initialise({}).isEmpty());

    CHECK(service->setDeviceType("ASIO").isNotEmpty());
    CHECK(service->setSampleRate(12345.0).isNotEmpty());
    CHECK(service->setBufferSize(100).isNotEmpty());
    CHECK(service->getStatus().isOpen); // still running on the previous device
    CHECK(service->getStatus().bufferSize == 256);
}

TEST_CASE("AudioDeviceService saves and restores the chosen device", "[engine][AudioDeviceService]")
{
    juce::ScopedJuceInitialiser_GUI juce;
    juce::String saved;
    {
        auto service = makeService();
        REQUIRE(service->initialise({}).isEmpty());
        REQUIRE(service->setDeviceType("Fake Exclusive").isEmpty());
        REQUIRE(service->setOutputDevice("HDMI").isEmpty());
        REQUIRE(service->setSampleRate(44100.0).isEmpty());
        REQUIRE(service->setBufferSize(512).isEmpty());
        saved = service->getStateXml();
    }
    REQUIRE(saved.isNotEmpty());

    auto restored = makeService(); // "after a restart"
    CHECK(restored->initialise(saved).isEmpty());
    const auto s = restored->getStatus();
    CHECK(s.typeName == "Fake Exclusive");
    CHECK(s.deviceName == "HDMI");
    CHECK(s.sampleRate == 44100.0);
    CHECK(s.bufferSize == 512);
}

TEST_CASE("AudioDeviceService falls back to the default when the saved device is gone", "[engine][AudioDeviceService]")
{
    juce::ScopedJuceInitialiser_GUI juce;
    const juce::String savedForMissingDevice =
        R"(<DEVICESETUP deviceType="Fake Exclusive" audioOutputDeviceName="Unplugged USB" audioInputDeviceName="" audioDeviceRate="48000.0" audioDeviceBufferSize="256"/>)";

    auto service = makeService();
    service->initialise(savedForMissingDevice);
    CHECK(service->getStatus().isOpen);

    auto garbage = makeService();
    garbage->initialise("this is not xml");
    CHECK(garbage->getStatus().isOpen);
}

TEST_CASE("AudioDeviceService reports latency", "[engine][AudioDeviceService]")
{
    juce::ScopedJuceInitialiser_GUI juce;
    auto service = makeService();
    REQUIRE(service->initialise({}).isEmpty());
    REQUIRE(service->setSampleRate(48000.0).isEmpty());
    REQUIRE(service->setBufferSize(256).isEmpty());

    const auto s = service->getStatus();
    CHECK(s.outputLatencySamples == 64); // fake driver latency
    CHECK(std::abs(s.bufferMs - 256.0 / 48.0) < 1e-9);
    CHECK(std::abs(s.outputLatencyMs - 64.0 / 48.0) < 1e-9);
    CHECK(std::abs(s.totalOutputLatencyMs - 320.0 / 48.0) < 1e-9);
}

TEST_CASE("AudioDeviceService counts xruns when the callback overruns its buffer time", "[engine][AudioDeviceService]")
{
    juce::ScopedJuceInitialiser_GUI juce;
    auto service = makeService();
    REQUIRE(service->initialise({}).isEmpty());
    REQUIRE(service->setSampleRate(48000.0).isEmpty());
    REQUIRE(service->setBufferSize(256).isEmpty());

    TestCallback callback;
    service->setAudioCallback(&callback);
    REQUIRE(waitFor([&] { return callback.calls > 20; }));
    CHECK(service->getStatus().xruns == 0);

    callback.slow = true;
    CHECK(waitFor([&] { return service->getStatus().xruns >= 3; }));
    CHECK(service->getStatus().cpuLoad >= 0.99); // JUCE caps the load reading at 100 %

    service->setAudioCallback(nullptr);
}

TEST_CASE("AudioDeviceService status description keeps non-ASCII device names intact", "[engine][AudioDeviceService]")
{
    devildj::engine::AudioDeviceStatus s;
    s.isOpen = true;
    s.typeName = "Windows Audio";
    s.deviceName = juce::String::fromUTF8(reinterpret_cast<const char*>(u8"스피커 (Realtek(R) Audio)"));
    s.sampleRate = 48000.0;
    s.bufferSize = 512;
    s.bufferMs = 10.666;
    s.totalOutputLatencyMs = 21.333;

    const auto text = devildj::engine::describeStatus(s);
    CHECK(text ==
          juce::String::fromUTF8(reinterpret_cast<const char*>(
              u8"Windows Audio / 스피커 (Realtek(R) Audio), 48000 Hz, 512 samples (10.7 ms), output latency 21.3 ms")));

    devildj::engine::AudioDeviceStatus closed;
    closed.lastError = "device unplugged";
    CHECK(devildj::engine::describeStatus(closed) == "no audio device open (device unplugged)");
}
