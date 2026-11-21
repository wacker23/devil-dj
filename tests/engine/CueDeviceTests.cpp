// P2-25: the headphone cue on a separate output device - the master on the speakers, the cue in the headphones.

#include "AudioSettingsWindow.h"
#include "engine/AudioDeviceService.h"
#include "engine/AudioEngine.h"
#include "engine/CueDevice.h"
#include "engine/OutputRouter.h"

#include "FakeAudioDevice.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

using namespace devildj;

TEST_CASE("Separate headphone device: the master stays on this device, the cue goes to the buffer", "[engine][cuedevice]")
{
    engine::AudioEngine e;
    e.prepare(48000.0, 256);
    e.send(engine::SetOutputLayout{engine::OutputLayout::SeparateCueDevice});
    float l[256], r[256];
    float* outs[] = {l, r};
    // A tone on the cue bus: nothing of it on the device's outputs; the buffer gets it.
    e.send(engine::SetTestTone{true, 440.0f, -12.0f, engine::Bus::Cue, 0.0f});
    for (int i = 0; i < 8; ++i)
        e.process(outs, 2, 256);
    float loudest = 0.0f;
    for (int i = 0; i < 256; ++i)
        loudest = std::max({loudest, std::abs(l[i]), std::abs(r[i])});
    CHECK(loudest == 0.0f);
    std::vector<float> cl(256), cr(256);
    e.separateCueOutput().read(cl.data(), cr.data(), 256);
    CHECK(e.separateCueOutput().playing());
    float cue = 0.0f;
    for (const float v : cl)
        cue = std::max(cue, std::abs(v));
    CHECK(cue > 0.1f);
    CHECK(e.readState().cueRouted);
    // A tone on the master: on the device's outputs as usual.
    e.send(engine::SetTestTone{true, 440.0f, -12.0f, engine::Bus::Master, 0.0f});
    for (int i = 0; i < 4; ++i)
        e.process(outs, 2, 256);
    loudest = 0.0f;
    for (int i = 0; i < 256; ++i)
        loudest = std::max(loudest, std::abs(l[i]));
    CHECK(loudest > 0.1f);
    // The router: master on 1/2, the other layouts as before.
    std::vector<float> m(64, 0.5f), c(64, -0.25f), o0(64), o1(64);
    float* routed[] = {o0.data(), o1.data()};
    engine::routeOutputs(engine::OutputLayout::SeparateCueDevice, m.data(), m.data(), c.data(), c.data(), routed, 2, 64);
    CHECK(o0[10] == 0.5f);
    CHECK(o1[10] == 0.5f);
    CHECK(engine::outputChannelsFor(engine::OutputLayout::SeparateCueDevice) == 2);
}

TEST_CASE("Separate headphone device: a second sound card plays the cue while the first plays the master",
          "[engine][cuedevice]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    engine::AudioDeviceService audio;
    audio.addDeviceType(std::make_unique<test::FakeAudioDeviceType>(
        "Fake Audio", juce::StringArray{"Speakers", "Headphones", "USB headset"}));
    REQUIRE(audio.initialise({}).isEmpty());
    REQUIRE(audio.setOutputDevice("Speakers").isEmpty());
    engine::AudioEngine e;
    audio.setAudioCallback(&e);
    e.send(engine::SetOutputLayout{engine::OutputLayout::SeparateCueDevice});

    engine::CueDevice phones(audio, e.separateCueOutput());
    // The other devices of the driver, not the master's.
    CHECK(phones.availableDevices() == juce::StringArray{"Headphones", "USB headset"});
    CHECK(phones.open("Speakers").isNotEmpty()); // the master's own device: no
    CHECK(phones.open("Nowhere").isNotEmpty());
    CHECK_FALSE(phones.status().open);

    REQUIRE(phones.open("Headphones").isEmpty());
    e.send(engine::SetTestTone{true, 440.0f, -12.0f, engine::Bus::Cue, 0.0f});
    // Both fake cards run their own callback threads; give them half a second.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && !(phones.status().playing && phones.status().peak > 0.1f))
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto s = phones.status();
    INFO("headphones: " << s.deviceName << ", playing " << s.playing << ", peak " << s.peak << ", delay " << s.delayMs
                        << " ms, dropouts " << s.dropouts);
    CHECK(s.open);
    CHECK(s.deviceName == "Headphones");
    CHECK(s.playing);
    CHECK(s.peak > 0.1f); // the tone (-12 dB = 0.25) reaches the headphones
    CHECK(s.delayMs > 0.0);
    CHECK(s.delayMs < 60.0);
    CHECK(s.dropouts == 0);

    // Another headphone device; then off.
    REQUIRE(phones.open("USB headset").isEmpty());
    CHECK(phones.status().deviceName == "USB headset");
    phones.close();
    CHECK_FALSE(phones.status().open);
    audio.setAudioCallback(nullptr);
}

TEST_CASE("Audio settings: the separate-headphones card lists the other devices and opens the chosen one",
          "[app][audio-settings][cuedevice]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    engine::AudioDeviceService audio;
    audio.addDeviceType(std::make_unique<test::FakeAudioDeviceType>(
        "Fake Audio", juce::StringArray{"Speakers", "Headphones", "USB headset"}));
    REQUIRE(audio.initialise({}).isEmpty());
    REQUIRE(audio.setOutputDevice("Speakers").isEmpty());
    engine::AudioEngine e;
    engine::CueDevice phones(audio, e.separateCueOutput());
    auto layout = engine::OutputLayout::StereoMaster;
    juce::String chosen;
    app::AudioSettingsComponent::Callbacks cb;
    cb.getLayout = [&] { return layout; };
    cb.setLayout = [&](engine::OutputLayout l)
    {
        layout = l;
        if (l == engine::OutputLayout::SeparateCueDevice)
            phones.open(chosen.isEmpty() ? phones.availableDevices()[0] : chosen);
    };
    cb.cueDevice = &phones;
    cb.setCueDevice = [&](const juce::String& name)
    {
        chosen = name;
        return phones.open(name);
    };
    app::AudioSettingsComponent page(audio, e, cb);
    page.setSize(860, 610);
    CHECK(page.cueDeviceBox().getNumItems() == 2);
    CHECK_FALSE(page.cueDeviceBox().isEnabled()); // another layout chosen
    page.layoutButton(engine::OutputLayout::SeparateCueDevice).onClick();
    CHECK(layout == engine::OutputLayout::SeparateCueDevice);
    CHECK(page.cueDeviceBox().isEnabled());
    CHECK(phones.status().deviceName == "Headphones");
    page.cueDeviceBox().setSelectedId(2, juce::sendNotificationSync); // "USB headset"
    CHECK(chosen == "USB headset");
    CHECK(phones.status().deviceName == "USB headset");
    // It paints (the card shows the headphone device's state).
    juce::Image image(juce::Image::ARGB, 860, 610, true);
    juce::Graphics g(image);
    page.paintEntireComponent(g, false);
    phones.close();
}
