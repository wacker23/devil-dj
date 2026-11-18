#include "MainComponent.h"

#include <taglib/tversionnumber.h>

namespace devildj::app
{

namespace
{
constexpr int kOverlayMargin = 12;
}

MainComponent::MainComponent(engine::AudioEngine& audioEngine, engine::AudioDeviceService& audio)
    : engine(audioEngine)
    , overlay([&audioEngine] { return audioEngine.readState(); }, [&audio] { return audio.getStatus(); })
{
    addChildComponent(overlay);
    setWantsKeyboardFocus(true);
    setSize(1280, 800);
}

// Temporary key handling until the KeyboardController binds keys by scan code (P1-16).
bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::F12Key)
    {
        toggleDebugOverlay();
        return true;
    }
    return false;
}

void MainComponent::toggleDebugOverlay()
{
    overlay.setVisible(!overlay.isVisible());
    if (overlay.isVisible())
        overlay.toFront(false);
}

void MainComponent::toggleTestTone()
{
    testToneOn = !testToneOn;
    engine.send(engine::SetTestTone{testToneOn, 440.0f, -18.0f});
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    g.setColour(juce::Colours::white.withAlpha(0.6f));
    g.setFont(juce::FontOptions(32.0f, juce::Font::bold));
    g.drawText("Devil-DJ", getLocalBounds(), juce::Justification::centred);

    // Temporary build-info line; moves to the About box later.
    const auto version = TagLib::runtimeVersion();
    const auto info = juce::String("JUCE ") + juce::String(JUCE_MAJOR_VERSION) + "." +
                      juce::String(JUCE_MINOR_VERSION) + "." + juce::String(JUCE_BUILDNUMBER) + "  |  TagLib " +
                      juce::String(version.majorVersion()) + "." + juce::String(version.minorVersion()) + "." +
                      juce::String(version.patchVersion());
    g.setColour(juce::Colours::white.withAlpha(0.35f));
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(info, getLocalBounds().reduced(12), juce::Justification::bottomLeft);
}

void MainComponent::resized()
{
    overlay.setBounds(getWidth() - ui::DebugOverlay::kWidth - kOverlayMargin, kOverlayMargin, ui::DebugOverlay::kWidth,
                      ui::DebugOverlay::kHeight);
}

} // namespace devildj::app
