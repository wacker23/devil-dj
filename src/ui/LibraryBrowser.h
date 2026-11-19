#pragma once

#include "ui/FileBrowserPanel.h"
#include "ui/TrackTableModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace devildj::library
{
class Connection;
class Database;
} // namespace devildj::library

namespace devildj::ui
{

// The library browser (P2-04), the bottom panel of the main window.
//   Left: a tree - Collection, Playlists (folders and playlists), Smart playlists, History, Folders - and the
//   selected track's cover art.
//   Right: the tracks of the selected tree item in a virtualized table (TrackTableModel: only visible rows are
//   read, a page of 200 at a time) with sortable, movable, resizable columns; right-click the header to show or
//   hide columns. The layout (order, widths, hidden columns, sort) is a string the app saves (columnLayout()).
//   "Folders" shows the file browser (FileBrowserPanel), for files that aren't in the library.
// Loading: Tab / Enter (actions -> loadSelected), LOAD A / LOAD B, a double-click (deck A), or drag a row onto a
// deck. Keys: Up / Down / Page Up / Page Down move the selection; Left goes to the tree (or collapses / goes to
// the parent there), Right expands or goes back to the table. Importing: the IMPORT button or dropping folders /
// files from Explorer on the browser (onImport; the app runs library::importTracks on a worker thread).
// The browser listens to the database's change notifications and refreshes itself on the message thread.
class LibraryBrowser final : public juce::Component,
                             public juce::FileDragAndDropTarget,
                             private juce::TableListBoxModel,
                             private juce::TableHeaderComponent::Listener,
                             private juce::AsyncUpdater
{
public:
    enum class View
    {
        Collection,
        Playlist,
        SmartPlaylist, // P2-07: rules, no tracks yet
        History,       // P2-23
        Folders,
    };

    // db may be null (the library couldn't be opened): only Folders works then, and `problem` says why.
    LibraryBrowser(library::Database* db, juce::File artworkFolder, juce::String problem = {});
    ~LibraryBrowser() override;

    std::function<void(int deck, const juce::File& file)> onLoad;
    std::function<void(const juce::String& layout)> onColumnLayoutChanged;
    std::function<void(const juce::Array<juce::File>& filesOrFolders)> onImport; // empty array: ask for a folder
    std::function<void()> onToggleSize; // the BIGGER / SMALLER button (Ctrl+B)
    void setBig(bool big);              // shows the button's state

    FileBrowserPanel& folders() noexcept { return fileBrowser; }
    TrackTableModel* model() noexcept { return tableModel.get(); }
    juce::TableListBox& table() noexcept { return trackTable; }
    juce::TreeView& tree() noexcept { return treeView; }

    // Column layout: juce::TableHeaderComponent::toString() (column order, widths, visibility, sort).
    juce::String columnLayout() const;
    void setColumnLayout(const juce::String& layout);
    // The header tells its listeners about moves, resizes and sort clicks asynchronously; this applies the
    // header's sort to the rows and reports the layout now (the app needn't call it; tests do).
    void syncWithHeader();

    juce::StringArray treeRowNames() const; // the tree's rows as shown, top to bottom

    View view() const noexcept { return currentView; }
    void showCollection();
    void showPlaylist(std::int64_t playlistId);
    void showFolders();
    std::int64_t selectedTrackId() const;
    juce::File selectedTrackFile() const;
    void selectTrack(std::int64_t trackId);
    void setStatus(const juce::String& text); // e.g. import progress
    juce::String status() const { return statusLabel.getText(); }
    bool artworkShown() const noexcept { return artwork.isValid(); }

    // Keyboard actions (the KeyboardController calls these through the ActionDispatcher).
    void moveSelection(int delta);
    int pageSize() const;
    bool loadSelected(int deck); // false if nothing is selected
    void collapse();             // Left
    void expand();               // Right
    bool treeHasFocus() const noexcept { return treeFocused; }

    // Re-reads tracks and playlists now (normally done after a change notification).
    void refreshFromLibrary(bool tracks, bool playlists);
    // Handles change notifications that are waiting for the message loop now (tests have no running loop).
    void handlePendingLibraryChanges() { handleUpdateNowIfNeeded(); }

    void resized() override;
    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;

    // Explorer drops: import.
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray&, int, int) override;
    void fileDragExit(const juce::StringArray&) override;
    void filesDropped(const juce::StringArray& files, int, int) override;

    class Item; // tree items

private:
    // TableListBoxModel
    int getNumRows() override;
    void paintRowBackground(juce::Graphics&, int row, int width, int height, bool selected) override;
    void paintCell(juce::Graphics&, int row, int column, int width, int height, bool selected) override;
    void sortOrderChanged(int newSortColumnId, bool isForwards) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void cellDoubleClicked(int row, int column, const juce::MouseEvent&) override;
    void cellClicked(int row, int column, const juce::MouseEvent&) override;
    juce::var getDragSourceDescription(const juce::SparseSet<int>& rows) override;
    juce::String getCellTooltip(int row, int column) override;

    // TableHeaderComponent::Listener
    void tableColumnsChanged(juce::TableHeaderComponent*) override;
    void tableColumnsResized(juce::TableHeaderComponent*) override;
    void tableSortOrderChanged(juce::TableHeaderComponent*) override;

    void handleAsyncUpdate() override;

    friend class Item;
    void itemSelected(View v, std::int64_t playlistId);
    void rebuildTree();
    void setView(View v, std::int64_t playlistId);
    void updateArtwork();
    void layoutChanged();
    void updateCount();

    library::Database* database;
    std::unique_ptr<library::Connection> reader;
    std::unique_ptr<TrackTableModel> tableModel;
    juce::File artworkDir;
    juce::String libraryProblem;

    juce::TreeView treeView;
    std::unique_ptr<juce::TreeViewItem> treeRoot;
    juce::TableListBox trackTable{{}, this};
    FileBrowserPanel fileBrowser;
    juce::TextButton importButton{"IMPORT"}, sizeButton{"BIGGER"}, loadA{"LOAD A"}, loadB{"LOAD B"};
    juce::Label statusLabel, countLabel;
    juce::Image artwork;
    juce::Rectangle<int> artworkArea;
    View currentView = View::Collection;
    std::int64_t currentPlaylist = 0;
    bool treeFocused = false;
    bool dropHover = false;
    bool restoringLayout = false;

    // Change notifications arrive on the database writer thread; this bridge outlives `this` safely.
    struct Bridge
    {
        std::mutex mutex;
        LibraryBrowser* owner = nullptr;
        bool tracks = false, playlists = false;
    };
    std::shared_ptr<Bridge> bridge;
    int listenerId = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LibraryBrowser)
};

} // namespace devildj::ui
