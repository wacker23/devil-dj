#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace devildj::ui
{

// Temporary file browser (P1-18) until the library (Phase 2): a place picker (Music, Desktop, Downloads,
// Documents, drives), a folder tree, and the audio files of the selected folder. Load the selected file with
// Tab (deck A) / Enter (deck B), the LOAD A / LOAD B buttons, or a double-click (deck A). Keyboard moves come
// in as actions (Up / Down / Page Up / Page Down, Left = parent folder) from the KeyboardController.
class FileBrowserPanel final : public juce::Component, private juce::FileBrowserListener
{
public:
    FileBrowserPanel();
    ~FileBrowserPanel() override;

    std::function<void(int deck, const juce::File& file)> onLoad;
    std::function<void(const juce::File& folder)> onFolderChanged; // remember it in the settings

    void setFolder(const juce::File& folder); // shows it in the tree and lists its audio files
    juce::File currentFolder() const;
    juce::File selectedFile() const;
    int numFiles() const;
    bool isStillLoading() const;
    void refresh(); // re-read both lists now (normally they update themselves on a change message)

    // Keyboard actions (the KeyboardController calls these through the ActionDispatcher).
    void moveSelection(int delta); // +1 / -1 rows, or +-page
    int pageSize() const;
    void goToParentFolder();
    bool loadSelected(int deck); // false if nothing is selected

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void selectionChanged() override;
    void fileClicked(const juce::File& file, const juce::MouseEvent& e) override;
    void fileDoubleClicked(const juce::File& file) override;
    void browserRootChanged(const juce::File& newRoot) override;
    void placeChosen();

    juce::TimeSliceThread thread{"File browser"};
    static juce::String audioPatterns(); // "*.mp3;*.wav;..." from engine::supportedTrackExtensions()
    juce::WildcardFileFilter folderFilter{"", "*", "Folders"};
    juce::WildcardFileFilter audioFilter{audioPatterns(), "", "Audio files"};
    juce::DirectoryContentsList folders{&folderFilter, thread};
    juce::DirectoryContentsList files{&audioFilter, thread};
    juce::FileTreeComponent folderTree{folders};
    juce::FileListComponent fileList{files};
    juce::ComboBox places;
    juce::Array<juce::File> placeFolders;
    juce::TextButton loadA{"LOAD A"}, loadB{"LOAD B"};
    juce::Label hint;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FileBrowserPanel)
};

} // namespace devildj::ui
