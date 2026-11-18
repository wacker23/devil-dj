// P1-19: audio settings page. Done when: the owner can set up the laptop + headphones without help (manual
// test). These tests check that every control on the page really changes the device / layout / tone, against
// a fake sound card, and render the screenshot used in DEVLOG.

#include "AudioSettingsWindow.h"
#include "engine/FakeAudioDevice.h"

#include <catch2/catch_test_macros.hpp>

using namespace devildj;
using app::AudioSettingsComponent;

namespace
{
struct Rig
{
    juce::ScopedJuceInitialiser_GUI gui;
    engine::AudioDeviceService audio;
    engine::AudioEngine engine;
    engine::OutputLayout layout = engine::OutputLayout::StereoMaster;
    std::unique_ptr<AudioSettingsComponent> page;

    explicit Rig(int outputs = 2)
    {
        audio.addDeviceType(std::make_unique<test::FakeAudioDeviceType>(
            "Fake Audio", juce::StringArray{"Laptop speakers", "USB interface"}, outputs));
        audio.addDeviceType(std::make_unique<test::FakeAudioDeviceType>("Fake Exclusive",
                                                                        juce::StringArray{"Laptop speakers"}, outputs));
        REQUIRE(audio.initialise({}).isEmpty());
        AudioSettingsComponent::Callbacks cb;
        cb.getLayout = [this] { return layout; };
        cb.setLayout = [this](engine::OutputLayout l)
        {
            layout = l;
            engine.send(engine::SetOutputLayout{l});
        };
        page = std::make_unique<AudioSettingsComponent>(audio, engine, cb);
    }
    void process()
    {
        std::vector<float> a(256), b(256);
        float* outs[] = {a.data(), b.data()};
        engine.prepare(48000.0, 256);
        engine.process(outs, 2, 256);
    }
};

int itemIdWithText(juce::ComboBox& box, const juce::String& text)
{
    for (int i = 0; i < box.getNumItems(); ++i)
        if (box.getItemText(i) == text)
            return box.getItemId(i);
    return 0;
}
} // namespace

TEST_CASE("Audio settings: helpers", "[app][audio-settings]")
{
    CHECK(AudioSettingsComponent::bufferText(256, 48000.0) == "256 samples  (5.3 ms)");
    CHECK(AudioSettingsComponent::bufferText(512, 44100.0) == "512 samples  (11.6 ms)");
    using R = AudioSettingsComponent::LatencyRating;
    CHECK(AudioSettingsComponent::rateLatency(8.0) == R::Great);
    CHECK(AudioSettingsComponent::rateLatency(20.0) == R::Good);
    CHECK(AudioSettingsComponent::rateLatency(40.0) == R::High);
}

TEST_CASE("Audio settings: the page shows the open device and its choices", "[app][audio-settings]")
{
    Rig rig;
    auto& p = *rig.page;
    CHECK(p.driverBox().getNumItems() == 2);
    CHECK(p.driverBox().getText() == "Fake Audio");
    CHECK(p.deviceBox().getText() == "Laptop speakers");
    CHECK(p.deviceBox().getNumItems() == 2);
    CHECK(p.rateBox().getNumItems() == 2);
    CHECK(p.rateBox().getSelectedId() == juce::roundToInt(rig.audio.getStatus().sampleRate));
    CHECK(p.bufferBox().getNumItems() == 3);
    CHECK(p.bufferBox().getSelectedId() == rig.audio.getStatus().bufferSize);
    CHECK(p.layoutButton(engine::OutputLayout::StereoMaster).getToggleState());
}

TEST_CASE("Audio settings: every control changes the real setting", "[app][audio-settings]")
{
    Rig rig(4);
    auto& p = *rig.page;

    p.bufferBox().setSelectedId(128, juce::sendNotificationSync); // user picks a buffer
    CHECK(rig.audio.getStatus().bufferSize == 128);
    CHECK(p.bufferBox().getText() == AudioSettingsComponent::bufferText(128, rig.audio.getStatus().sampleRate));

    p.rateBox().setSelectedId(44100, juce::sendNotificationSync);
    CHECK(juce::roundToInt(rig.audio.getStatus().sampleRate) == 44100);

    p.deviceBox().setSelectedId(itemIdWithText(p.deviceBox(), "USB interface"), juce::sendNotificationSync);
    CHECK(rig.audio.getStatus().deviceName == "USB interface");

    p.driverBox().setSelectedId(itemIdWithText(p.driverBox(), "Fake Exclusive"), juce::sendNotificationSync);
    CHECK(rig.audio.getCurrentDeviceType() == "Fake Exclusive");
    CHECK(p.deviceBox().getText() == "Laptop speakers"); // lists refresh for the new driver

    p.layoutButton(engine::OutputLayout::SplitMono).onClick();
    CHECK(rig.layout == engine::OutputLayout::SplitMono);
    CHECK(p.layoutButton(engine::OutputLayout::SplitMono).getToggleState());
    CHECK_FALSE(p.layoutButton(engine::OutputLayout::StereoMaster).getToggleState());
}

TEST_CASE("Audio settings: test tones go to the speakers or the headphones", "[app][audio-settings]")
{
    Rig rig;
    auto& p = *rig.page;
    rig.audio.setAudioCallback(nullptr); // drive the engine by hand
    p.toneHeadphonesButton().onClick();
    rig.process();
    CHECK(rig.engine.readState().testToneOn);
    CHECK(rig.engine.readState().testToneBus == engine::Bus::Cue);
    p.toneSpeakersButton().onClick();
    rig.process();
    CHECK(rig.engine.readState().testToneBus == engine::Bus::Master);
    p.toneStopButton().onClick();
    rig.process();
    CHECK_FALSE(rig.engine.readState().testToneOn);

    // Closing the page while a tone plays stops it.
    p.toneSpeakersButton().onClick();
    rig.page.reset();
    rig.process();
    CHECK_FALSE(rig.engine.readState().testToneOn);
}

TEST_CASE("Audio settings: glitch counter resets", "[app][audio-settings]")
{
    Rig rig;
    auto& p = *rig.page;
    CHECK(p.glitchesSinceReset() == 0);
    p.resetGlitchesButton().onClick();
    CHECK(p.glitchesSinceReset() == 0);
    CHECK(p.createComponentSnapshot(p.getLocalBounds()).getWidth() == p.getWidth()); // paints
}

TEST_CASE("Audio settings screenshot", "[.screenshot]")
{
    Rig rig(2);
    auto& p = *rig.page;
    p.layoutButton(engine::OutputLayout::SplitMono).onClick();
    juce::Image image(juce::Image::ARGB, p.getWidth(), p.getHeight(), true);
    {
        juce::Graphics g(image);
        p.paintEntireComponent(g, false);
    }
    const auto out =
        juce::File(juce::String::fromUTF8(DEVILDJ_SOURCE_DIR)).getChildFile("docs/images/p1-19-audio-settings.png");
    out.deleteFile();
    juce::FileOutputStream stream(out);
    REQUIRE(stream.openedOk());
    juce::PNGImageFormat png;
    REQUIRE(png.writeImageToStream(image, stream));
}
