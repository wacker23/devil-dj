#pragma once

#include "ui/FileBrowserPanel.h"
#include "ui/TrackTableModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "library/PlaylistRepository.h"

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
    std::function<void()> onSearchDone; // Enter / Esc in the search box: give the keys back to the decks

    // Search and filters (P2-05). Typing in the search box filters at once; FILTER shows a row with BPM range,
    // key, minimum rating, colour, genre and tag. Ctrl+F (focusSearch) jumps to the search box.
    void focusSearch();
    juce::TextEditor& searchBox() noexcept { return search; }
    void setFiltersShown(bool shown);
    bool filtersShown() const noexcept { return showFilters; }
    void setFilter(const TrackFilter& filter); // sets the controls and applies it
    TrackFilter filterFromControls() const;
    void clearFilters();
    juce::String countText() const { return countLabel.getText(); }
    juce::ComboBox& keyFilter() noexcept { return keyBox; }
    juce::ComboBox& genreFilter() noexcept { return genreBox; }
    juce::ComboBox& tagFilter() noexcept { return tagBox; }
    static juce::String colourName(std::uint32_t rgb); // "red", "blue", ... for the colour filter
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

    // Playlists (P2-06). Right-click the tree for new playlist / folder, rename, delete, M3U import / export;
    // right-click tracks for add to playlist / remove; drag tracks onto a playlist in the tree; drag rows inside a
    // playlist to reorder; Delete removes the selected rows from the shown playlist. Writes go through the
    // database writer; the tree and table follow its change notifications.
    std::function<void(std::optional<std::int64_t> parentFolder)> onImportPlaylist; // ask for an .m3u / .m3u8
    void createPlaylist(const juce::String& name, std::optional<std::int64_t> parentFolder, bool folder = false);
    void renamePlaylist(std::int64_t playlistId, const juce::String& name);
    void deletePlaylist(std::int64_t playlistId); // with everything inside a folder
    void addTracksToPlaylist(std::int64_t playlistId, const std::vector<std::int64_t>& trackIds);
    bool removeSelectedFromPlaylist();          // false when the view isn't a playlist or nothing is selected
    void moveSelectedRows(int insertBeforeRow); // reorder inside the shown playlist (0..rows)
    juce::String exportPlaylist(std::int64_t playlistId, const juce::File& file); // error, empty when written
    std::vector<int> selectedRows() const;
    std::vector<std::int64_t> selectedTrackIds() const;
    std::int64_t shownPlaylist() const noexcept; // the playlist in the table, 0 for the collection
    bool canReorder() const noexcept;            // a normal playlist, sorted by #, no filter
    juce::String uniquePlaylistName(const juce::String& base, std::optional<std::int64_t> parentFolder);
    juce::PopupMenu menuForTree(std::int64_t playlistId, View section); // what a right-click shows

    // Ratings, colours and tags (P2-08), on the selected tracks. Ctrl+Shift+1..5 / 0 rate; a click on the
    // stars in the Rating column rates (the same star again clears); a click on the colour cell opens the
    // palette; right-click -> Rating / Colour / Tags. Tags: categories with tags, any number per track.
    void rateSelected(int stars);
    void colourSelected(std::optional<std::uint32_t> rgb);
    void setTagOnSelected(std::int64_t tagId, bool on);
    void createTag(const juce::String& name, std::optional<std::int64_t> categoryId, bool assignToSelected);
    void createTagCategory(const juce::String& name);
    void renameTag(std::int64_t tagId, const juce::String& name);
    void deleteTag(std::int64_t tagId);
    void renameTagCategory(std::int64_t categoryId, const juce::String& name);
    void deleteTagCategory(std::int64_t categoryId);
    juce::PopupMenu ratingMenu();
    juce::PopupMenu colourMenu();
    juce::PopupMenu tagMenu();
    static int starAt(int xInCell); // which star (1..5) a click in the Rating cell hits, 0 = left of them

    // Smart playlists (P2-07): rules in rule_json; the table always shows what they match right now.
    void createSmartPlaylist(const juce::String& name, const library::SmartRules& rules);
    void updateSmartPlaylist(std::int64_t playlistId, const juce::String& name, const library::SmartRules& rules);
    void editSmartPlaylist(std::int64_t playlistId); // 0 = a new one (opens the rule editor)
    juce::PopupMenu menuForTracks();

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
    void showTreeMenu(std::int64_t playlistId, View section);
    void askName(const juce::String& title, const juce::String& initial, std::function<void(juce::String)> done);
    void confirmDelete(std::int64_t playlistId);
    void chooseExportFile(std::int64_t playlistId);
    std::optional<library::Playlist> playlistInfo(std::int64_t playlistId);
    std::int64_t selectAfterRebuild = 0;
    std::vector<int> selectRowsAfterReload; // rows moved by a drag: selected again once the new order is read
    std::unique_ptr<juce::FileChooser> exportChooser;

    // The track table, also a drop target for reordering rows inside a playlist.
    class TrackTable final : public juce::TableListBox, public juce::DragAndDropTarget
    {
    public:
        explicit TrackTable(LibraryBrowser& b)
            : juce::TableListBox({}, &b)
            , owner(b)
        {
        }
        bool isInterestedInDragSource(const SourceDetails& d) override
        {
            return owner.canReorder() && d.sourceComponent != nullptr &&
                   (d.sourceComponent.get() == this || isParentOf(d.sourceComponent.get()));
        }
        void itemDragMove(const SourceDetails& d) override
        {
            const int row = getInsertionIndexForPosition(d.localPosition.x, d.localPosition.y);
            if (row != insertRow)
            {
                insertRow = row;
                repaint();
            }
        }
        void itemDragExit(const SourceDetails&) override
        {
            insertRow = -1;
            repaint();
        }
        void itemDropped(const SourceDetails& d) override
        {
            const int row = getInsertionIndexForPosition(d.localPosition.x, d.localPosition.y);
            insertRow = -1;
            repaint();
            owner.moveSelectedRows(row);
        }
        void paintOverChildren(juce::Graphics& g) override
        {
            juce::TableListBox::paintOverChildren(g);
            if (insertRow < 0)
                return;
            const auto r = getRowPosition(std::min(insertRow, getNumRows() - 1), true);
            const int y = insertRow >= getNumRows() ? r.getBottom() : r.getY();
            g.setColour(juce::Colour(0xffe0483c));
            g.fillRect(0, y - 1, getWidth(), 3);
        }

    private:
        LibraryBrowser& owner;
        int insertRow = -1;
    };
    void rebuildTree();
    void setView(View v, std::int64_t playlistId);
    void updateArtwork();
    void layoutChanged();
    void updateCount();
    void applyFilter();
    void refreshFilterChoices(); // genres, tags and colours that exist in the library

    library::Database* database;
    std::unique_ptr<library::Connection> reader;
    std::unique_ptr<TrackTableModel> tableModel;
    juce::File artworkDir;
    juce::String libraryProblem;

    juce::TreeView treeView;
    std::unique_ptr<juce::TreeViewItem> treeRoot;
    TrackTable trackTable{*this};
    FileBrowserPanel fileBrowser;
    juce::TextButton importButton{"IMPORT"}, sizeButton{"BIGGER"}, loadA{"LOAD A"}, loadB{"LOAD B"};
    juce::Label statusLabel, countLabel;
    juce::TextEditor search, bpmFrom, bpmTo;
    juce::TextButton filterButton{"FILTER"}, clearButton{"CLEAR"};
    juce::ComboBox keyBox, ratingBox, colourBox, genreBox, tagBox;
    juce::Label bpmLabel, bpmDash;
    std::vector<std::uint32_t> colourChoices;
    std::vector<std::int64_t> tagChoices;
    bool showFilters = false;
    bool settingControls = false;
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
