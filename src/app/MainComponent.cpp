#include "MainComponent.h"

#include "engine/TrackDecoder.h"

#include <taglib/tversionnumber.h>

namespace devildj::app
{

namespace
{
constexpr int kOverlayMargin = 12;
}

MainComponent::MainComponent(engine::AudioEngine& audioEngine, engine::AudioDeviceService& audioService,
                             engine::TrackLoader& trackLoader, engine::BufferRegistry& bufferRegistry)
    : engine(audioEngine)
    , audio(audioService)
    , loader(trackLoader)
    , registry(bufferRegistry)
    , overlay([&audioEngine] { return audioEngine.readState(); }, [&audioService] { return audioService.getStatus(); })
{
    overlay.setExtraLines([this] { return statusLines(); });
    addChildComponent(overlay);

    loader.callbacks.onProgress = [this](engine::DeckId deck, const juce::File& file, double fraction)
    {
        if (deck < deckStatus.size())
            deckStatus[deck] =
                "loading " + file.getFileName() + " " + juce::String(juce::roundToInt(100 * fraction)) + " %";
    };
    loader.callbacks.onLoaded = [this](engine::DeckId deck, const engine::TrackBuffer& t)
    {
        if (deck < deckStatus.size())
            deckStatus[deck] =
                t.file.getFileName() + " (" + juce::String(t.durationSeconds(), 1) + " s, " + t.formatName + ")";
    };
    loader.callbacks.onError = [this](engine::DeckId deck, const juce::File& file, const juce::String& error)
    {
        if (deck < deckStatus.size())
            deckStatus[deck] = "failed: " + file.getFileName() + " - " + error;
    };
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

MainComponent::~MainComponent()
{
    loader.callbacks = {}; // the loader outlives this component; don't let late results call into it
}

void MainComponent::chooseTrackForDeck(engine::DeckId deck)
{
    juce::StringArray patterns;
    for (const auto& ext : engine::supportedTrackExtensions())
        patterns.add("*" + ext);

    chooser = std::make_unique<juce::FileChooser>(juce::String("Load track to deck ") + engine::deckLetter(deck),
                                                  juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                  patterns.joinIntoString(";"));
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this, deck](const juce::FileChooser& fc)
                         {
                             const auto file = fc.getResult();
                             if (file == juce::File())
                                 return;
                             deckStatus[deck] = "loading " + file.getFileName();
                             loader.load(deck, file, audio.getStatus().sampleRate);
                         });
}

void MainComponent::unloadDeck(engine::DeckId deck)
{
    loader.unload(deck);
    deckStatus[deck] = "empty";
}

juce::StringArray MainComponent::statusLines() const
{
    juce::StringArray lines;
    for (engine::DeckId d = 0; d < deckStatus.size(); ++d)
        lines.add(juce::String("Deck ") + engine::deckLetter(d) + ": " + deckStatus[d]);
    lines.add("Track buffers: " + juce::String(engine::TrackBuffer::liveCount()) + " alive, " +
              juce::String(static_cast<int>(registry.size())) + " registered, " +
              juce::String(juce::roundToInt(static_cast<double>(registry.memoryBytes()) / (1024.0 * 1024.0))) + " MB");
    return lines;
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
