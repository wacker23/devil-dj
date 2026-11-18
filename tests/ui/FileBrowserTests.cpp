// P1-18: simple file loading. Done when: drag-and-drop and keyboard loading both work.
// Keyboard: Tab / Enter perform DeckA.Load / DeckB.Load, whose handler calls FileBrowserPanel::loadSelected;
// Up / Down call moveSelection. Drag-and-drop: Explorer drops arrive through juce::FileDragAndDropTarget on
// the deck panel and its waveform (TrackDropTarget), tested here with the same calls JUCE makes.

#include "TestData.h"
#include "engine/AudioEngine.h"
#include "ui/DeckView.h"
#include "ui/FileBrowserPanel.h"
#include "ui/WaveformView.h"

#include <catch2/catch_test_macros.hpp>

#include <thread>

using namespace devildj;

namespace
{
struct TempLibrary
{
    juce::TemporaryFile dir{juce::String(), juce::File::getSpecialLocation(juce::File::tempDirectory)};
    juce::File root, sub, wav, mp3, notes;

    TempLibrary()
    {
        root = dir.getFile();
        root.createDirectory();
        sub = root.getChildFile("Sets");
        sub.createDirectory();
        const auto source = test::generatedFile("formats/ref.wav");
        wav = root.getChildFile("A - first track.wav");
        mp3 = root.getChildFile(juce::String::fromUTF8("B - 두번째 트랙.mp3")); // Korean file name
        notes = root.getChildFile("notes.txt");
        if (source.existsAsFile())
        {
            source.copyFileTo(wav);
            test::generatedFile("formats/ref.mp3").copyFileTo(mp3);
        }
        notes.replaceWithText("not audio");
        sub.getChildFile("C - in a folder.flac").replaceWithText("x");
    }
    ~TempLibrary() { root.deleteRecursively(); }
    bool ok() const { return wav.existsAsFile() && mp3.existsAsFile(); }
};

void waitUntilListed(ui::FileBrowserPanel& panel)
{
    for (int i = 0; i < 200 && panel.isStillLoading(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
} // namespace

TEST_CASE("File browser: lists the folder's audio files only", "[ui][browser]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempLibrary lib;
    if (!lib.ok())
        SKIP("Generated test signals missing");
    ui::FileBrowserPanel panel;
    panel.setSize(1200, 200);
    juce::File remembered;
    panel.onFolderChanged = [&](const juce::File& f) { remembered = f; };
    panel.setFolder(lib.root);
    waitUntilListed(panel);
    CHECK(panel.currentFolder() == lib.root);
    CHECK(remembered == lib.root);
    CHECK(panel.numFiles() == 2); // the .wav and the .mp3; not notes.txt, not the folder
}

TEST_CASE("File browser: keyboard select and load to A / B", "[ui][browser]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempLibrary lib;
    if (!lib.ok())
        SKIP("Generated test signals missing");
    ui::FileBrowserPanel panel;
    panel.setSize(1200, 200);
    std::vector<std::pair<int, juce::File>> loads;
    panel.onLoad = [&](int deck, const juce::File& f) { loads.emplace_back(deck, f); };
    panel.setFolder(lib.root);
    waitUntilListed(panel);
    panel.refresh(); // in the app a change message does this; tests have no message loop

    CHECK_FALSE(panel.loadSelected(0)); // nothing selected yet: the app falls back to the file chooser
    panel.moveSelection(1);             // Down: first file
    CHECK(panel.selectedFile() == lib.wav);
    CHECK(panel.loadSelected(0)); // Tab
    panel.moveSelection(1);       // Down
    CHECK(panel.selectedFile() == lib.mp3);
    CHECK(panel.loadSelected(1)); // Enter
    panel.moveSelection(5);       // past the end: stays on the last
    CHECK(panel.selectedFile() == lib.mp3);
    panel.moveSelection(-10); // Page Up past the top
    CHECK(panel.selectedFile() == lib.wav);
    REQUIRE(loads.size() == 2);
    CHECK(loads[0] == std::pair<int, juce::File>{0, lib.wav});
    CHECK(loads[1] == std::pair<int, juce::File>{1, lib.mp3});

    panel.setFolder(lib.sub);
    panel.goToParentFolder(); // Left
    CHECK(panel.currentFolder() == lib.root);
}

TEST_CASE("Drag and drop from Explorer onto a deck panel or its waveform", "[ui][browser][dragdrop]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempLibrary lib;
    if (!lib.ok())
        SKIP("Generated test signals missing");
    engine::AudioEngine engine;
    engine.prepare(48000.0, 256);
    ui::DeckView deck(engine, 1, [](engine::BufferId) { return nullptr; });
    ui::WaveformView wave(engine, 0, [](engine::BufferId) { return nullptr; }, [] { return 0.0; });
    deck.setSize(600, 250);
    wave.setSize(1200, 120);

    std::vector<juce::File> dropped;
    deck.onTrackDropped = [&](const juce::File& f) { dropped.push_back(f); };
    wave.onTrackDropped = [&](const juce::File& f) { dropped.push_back(f); };

    const juce::StringArray audio{lib.notes.getFullPathName(), lib.mp3.getFullPathName()};
    const juce::StringArray notAudio{lib.notes.getFullPathName(), lib.sub.getFullPathName()};
    CHECK(deck.isInterestedInFileDrag(audio));
    CHECK_FALSE(deck.isInterestedInFileDrag(notAudio)); // Explorer shows "no entry"

    // What JUCE calls while the user drags over the deck and lets go.
    deck.fileDragEnter(audio, 10, 10);
    CHECK(deck.isDropHovering());
    CHECK(deck.createComponentSnapshot(deck.getLocalBounds()).getWidth() == 600); // paints the highlight
    deck.filesDropped(audio, 10, 10);
    CHECK_FALSE(deck.isDropHovering());
    wave.fileDragEnter(audio, 5, 5);
    wave.fileDragExit(audio); // dragged away again: nothing loads
    CHECK_FALSE(wave.isDropHovering());
    wave.filesDropped(juce::StringArray{lib.wav.getFullPathName()}, 5, 5);

    REQUIRE(dropped.size() == 2);
    CHECK(dropped[0] == lib.mp3); // the first audio file of the drop (notes.txt skipped)
    CHECK(dropped[1] == lib.wav);
}

TEST_CASE("File browser screenshot", "[.screenshot]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TempLibrary lib;
    for (const auto* name : {"Basement Lights.mp3", "Night Drive (Extended Mix).flac", "Sunrise Edit.wav"})
        lib.root.getChildFile(name).replaceWithText("x");
    ui::FileBrowserPanel panel;
    panel.setSize(1256, 190);
    panel.setFolder(lib.root);
    waitUntilListed(panel);
    panel.refresh();
    for (auto* c : panel.getChildren()) // no user name from the temp path in a public screenshot
        if (auto* box = dynamic_cast<juce::ComboBox*>(c))
            box->setText("Music\\Sets", juce::dontSendNotification);
    panel.moveSelection(1);
    panel.moveSelection(1);
    juce::Image image(juce::Image::ARGB, 1280, 214, true);
    {
        juce::Graphics g(image);
        g.fillAll(juce::Colour(0xff101014));
        g.setOrigin(12, 12);
        panel.paintEntireComponent(g, false);
    }
    const auto out =
        juce::File(juce::String::fromUTF8(DEVILDJ_SOURCE_DIR)).getChildFile("docs/images/p1-18-browser.png");
    out.deleteFile();
    juce::FileOutputStream stream(out);
    REQUIRE(stream.openedOk());
    juce::PNGImageFormat png;
    REQUIRE(png.writeImageToStream(image, stream));
}
