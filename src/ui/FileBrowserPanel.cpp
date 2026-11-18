#include "FileBrowserPanel.h"

#include "engine/TrackDecoder.h"

#include <algorithm>

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kText(0xffd8d8e0);
const juce::Colour kAccent(0xffe0483c);
} // namespace

juce::String FileBrowserPanel::audioPatterns()
{
    juce::StringArray patterns;
    for (const auto& ext : engine::supportedTrackExtensions())
        patterns.add("*" + ext);
    return patterns.joinIntoString(";");
}

FileBrowserPanel::FileBrowserPanel()
{
    thread.startThread(juce::Thread::Priority::low);

    // Places: the usual folders, then every drive.
    const std::pair<const char*, juce::File::SpecialLocationType> named[] = {
        {"Music", juce::File::userMusicDirectory},
        {"Downloads", juce::File::userHomeDirectory}, // + "Downloads" below
        {"Desktop", juce::File::userDesktopDirectory},
        {"Documents", juce::File::userDocumentsDirectory},
        {"Home", juce::File::userHomeDirectory},
    };
    for (const auto& [name, where] : named)
    {
        auto folder = juce::File::getSpecialLocation(where);
        if (juce::String(name) == "Downloads")
            folder = folder.getChildFile("Downloads");
        if (folder.isDirectory())
        {
            placeFolders.add(folder);
            places.addItem(name, placeFolders.size());
        }
    }
    juce::Array<juce::File> roots;
    juce::File::findFileSystemRoots(roots);
    for (const auto& root : roots)
        if (root.isDirectory())
        {
            placeFolders.add(root);
            places.addItem(root.getFullPathName(), placeFolders.size());
        }
    places.setTextWhenNothingSelected("Places");
    places.onChange = [this] { placeChosen(); };
    places.setWantsKeyboardFocus(false);

    folderTree.addListener(this);
    fileList.addListener(this);
    folderTree.setWantsKeyboardFocus(false); // keys arrive as actions (KeyboardController)
    fileList.setWantsKeyboardFocus(false);
    for (auto* view : {static_cast<juce::Component*>(&folderTree), static_cast<juce::Component*>(&fileList)})
    {
        view->setColour(juce::DirectoryContentsDisplayComponent::highlightColourId, kAccent.withAlpha(0.55f));
        view->setColour(juce::DirectoryContentsDisplayComponent::textColourId, kText);
        view->setColour(juce::DirectoryContentsDisplayComponent::highlightedTextColourId, juce::Colours::white);
        view->setColour(juce::ListBox::backgroundColourId, kPanel);
        view->setColour(juce::TreeView::backgroundColourId, kPanel);
    }

    for (auto* b : {&loadA, &loadB})
    {
        b->setColour(juce::TextButton::buttonColourId, kPanel);
        b->setColour(juce::TextButton::textColourOffId, kText);
        b->setWantsKeyboardFocus(false);
    }
    loadA.onClick = [this] { loadSelected(0); };
    loadB.onClick = [this] { loadSelected(1); };
    loadA.setTooltip("Load the selected file to deck A (Tab)");
    loadB.setTooltip("Load the selected file to deck B (Enter)");

    hint.setText("Tab: load to A   Enter: load to B   Up / Down: select   Left: parent folder   "
                 "or drag files from Explorer onto a deck",
                 juce::dontSendNotification);
    hint.setColour(juce::Label::textColourId, kText.withAlpha(0.55f));
    hint.setFont(juce::FontOptions(12.0f));

    for (auto* c : std::initializer_list<juce::Component*>{&places, &folderTree, &fileList, &loadA, &loadB, &hint})
        addAndMakeVisible(c);

    if (!placeFolders.isEmpty())
        setFolder(placeFolders.getFirst());
}

FileBrowserPanel::~FileBrowserPanel()
{
    folderTree.removeListener(this);
    fileList.removeListener(this);
    thread.stopThread(2000);
}

void FileBrowserPanel::setFolder(const juce::File& folder)
{
    if (!folder.isDirectory())
        return;
    folders.setDirectory(folder, true, false);
    files.setDirectory(folder, false, true);
    places.setText(folder.getFullPathName(), juce::dontSendNotification); // a place's name if it is one
    for (int i = 0; i < placeFolders.size(); ++i)
        if (placeFolders[i] == folder)
            places.setSelectedId(i + 1, juce::dontSendNotification);
    if (onFolderChanged)
        onFolderChanged(folder);
    repaint();
}

juce::File FileBrowserPanel::currentFolder() const
{
    return files.getDirectory();
}

juce::File FileBrowserPanel::selectedFile() const
{
    return fileList.getNumSelectedFiles() > 0 ? fileList.getSelectedFile(0) : juce::File();
}

int FileBrowserPanel::numFiles() const
{
    return files.getNumFiles();
}

void FileBrowserPanel::refresh()
{
    folderTree.refresh();
    fileList.updateContent();
}

bool FileBrowserPanel::isStillLoading() const
{
    return files.isStillLoading() || folders.isStillLoading();
}

int FileBrowserPanel::pageSize() const
{
    return std::max(1, fileList.getHeight() / std::max(1, fileList.getRowHeight()) - 1);
}

void FileBrowserPanel::moveSelection(int delta)
{
    const int n = files.getNumFiles();
    if (n == 0)
        return;
    const int current = fileList.getSelectedRow();
    const int row = std::clamp(current < 0 ? (delta > 0 ? 0 : n - 1) : current + delta, 0, n - 1);
    fileList.selectRow(row);
}

void FileBrowserPanel::goToParentFolder()
{
    const auto parent = currentFolder().getParentDirectory();
    if (parent != currentFolder())
        setFolder(parent);
}

bool FileBrowserPanel::loadSelected(int deck)
{
    const auto file = selectedFile();
    if (!file.existsAsFile())
        return false;
    if (onLoad)
        onLoad(deck, file);
    return true;
}

void FileBrowserPanel::placeChosen()
{
    const int index = places.getSelectedId() - 1;
    if (index >= 0 && index < placeFolders.size())
        setFolder(placeFolders[index]);
}

void FileBrowserPanel::selectionChanged()
{
    // A folder picked in the tree: list its files (the tree stays rooted where it is).
    const auto folder = folderTree.getSelectedFile();
    if (folder.isDirectory() && folder != files.getDirectory())
        files.setDirectory(folder, false, true);
    repaint();
}

void FileBrowserPanel::fileClicked(const juce::File&, const juce::MouseEvent&) {}

void FileBrowserPanel::fileDoubleClicked(const juce::File& file)
{
    if (file.existsAsFile() && onLoad)
        onLoad(0, file);
}

void FileBrowserPanel::browserRootChanged(const juce::File&) {}

void FileBrowserPanel::resized()
{
    auto area = getLocalBounds().reduced(8);
    auto top = area.removeFromTop(26);
    loadB.setBounds(top.removeFromRight(80));
    top.removeFromRight(6);
    loadA.setBounds(top.removeFromRight(80));
    top.removeFromRight(10);
    places.setBounds(top.removeFromLeft(220));
    top.removeFromLeft(10);
    hint.setBounds(top);
    area.removeFromTop(6);
    folderTree.setBounds(area.removeFromLeft(std::min(320, area.getWidth() / 3)));
    area.removeFromLeft(6);
    fileList.setBounds(area);
}

void FileBrowserPanel::paint(juce::Graphics& g)
{
    g.setColour(kBackground);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
    g.setColour(kAccent.withAlpha(0.8f));
    g.fillRect(getLocalBounds().removeFromTop(3).reduced(8, 0).toFloat());
}

} // namespace devildj::ui
