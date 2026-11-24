#include "LibraryBrowser.h"

#include "engine/TrackDecoder.h"
#include "library/Connection.h"
#include "library/Database.h"
#include "library/HistoryRepository.h"
#include "library/PlaylistRepository.h"
#include "library/MissingFiles.h"
#include "library/PlaylistFiles.h"
#include "library/SmartRules.h"
#include "ui/SmartPlaylistEditor.h"
#include "library/TagRepository.h"
#include "library/TrackRepository.h"
#include "ui/TrackColours.h"

#include <algorithm>
#include <map>

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kRowAlt(0xff23232b);
const juce::Colour kText(0xffd8d8e0);
const juce::Colour kDim(0xff8a8a96);
const juce::Colour kAccent(0xffe0483c);

constexpr int kRowHeight = 22;
constexpr int kTreeWidth = 210;
constexpr int kToolbarHeight = 26;
constexpr int kFilterRowHeight = 26;
constexpr int kStarLeft = 6;   // the Rating cell's stars: where the first starts
constexpr int kStarWidth = 13; // and how wide each one is

struct ColumnInfo
{
    TrackColumn id;
    const char* name;
    int width;
    bool visible;
};

const ColumnInfo kColumns[] = {
    {TrackColumn::Number, "#", 44, true},
    {TrackColumn::Colour, "Colour", 26, true},
    {TrackColumn::Title, "Title", 260, true},
    {TrackColumn::Artist, "Artist", 180, true},
    {TrackColumn::Album, "Album", 160, false},
    {TrackColumn::Bpm, "BPM", 56, true},
    {TrackColumn::Key, "Key", 44, true},
    {TrackColumn::Time, "Time", 52, true},
    {TrackColumn::Rating, "Rating", 76, true},
    {TrackColumn::Genre, "Genre", 120, true},
    {TrackColumn::DateAdded, "Added", 84, true},
    {TrackColumn::Comment, "Comment", 200, true},
    {TrackColumn::Tags, "Tags", 160, true},
    {TrackColumn::Mix, "Mix", 110, true}, // P4-19
};

juce::String stars(int rating)
{
    juce::String s;
    for (int i = 1; i <= 5; ++i)
        s += i <= rating ? juce::String::fromUTF8("\xE2\x98\x85") : juce::String::fromUTF8("\xC2\xB7"); // ★ ·
    return s;
}

juce::String dateText(std::int64_t unixSeconds)
{
    if (unixSeconds <= 0)
        return {};
    return juce::Time(unixSeconds * 1000).formatted("%Y-%m-%d");
}
} // namespace

// --- tree items ---------------------------------------------------------------------------------------------------

class LibraryBrowser::Item final : public juce::TreeViewItem
{
public:
    Item(LibraryBrowser& b, View v, juce::String label, std::int64_t id = 0, bool folder = false)
        : browser(b)
        , itemView(v)
        , name(std::move(label))
        , playlistId(id)
        , isFolder(folder)
    {
    }

    bool mightContainSubItems() override { return getNumSubItems() > 0; }
    juce::String getUniqueName() const override { return juce::String(static_cast<int>(itemView)) + ":" + juce::String(playlistId) + ":" + name; }
    int getItemHeight() const override { return kRowHeight; }
    bool canBeSelected() const override { return !isFolder; }

    void paintItem(juce::Graphics& g, int width, int height) override
    {
        if (isSelected())
        {
            g.setColour(kAccent.withAlpha(browser.treeFocused ? 0.6f : 0.3f));
            g.fillRect(0, 0, width, height);
        }
        g.setColour(isFolder || getParentItem() == browser.treeRoot.get() ? kText : kText.withAlpha(0.85f));
        g.setFont(juce::FontOptions(14.0f, getParentItem() == browser.treeRoot.get() ? juce::Font::bold
                                                                                      : juce::Font::plain));
        g.drawText(name, 4, 0, width - 6, height, juce::Justification::centredLeft, true);
    }

    void itemClicked(const juce::MouseEvent& e) override
    {
        browser.treeFocused = true;
        if (e.mods.isPopupMenu())
            browser.showTreeMenu(playlistId, itemView);
    }

    // Tracks dragged from the table onto a playlist (P2-06).
    bool holdsTracks() const { return itemView == View::Playlist && !isFolder && playlistId > 0; }
    bool isInterestedInDragSource(const juce::DragAndDropTarget::SourceDetails& d) override
    {
        const auto* o = d.description.getDynamicObject();
        return holdsTracks() && o != nullptr && o->hasProperty("trackIds");
    }
    void itemDropped(const juce::DragAndDropTarget::SourceDetails& d, int) override
    {
        std::vector<std::int64_t> ids;
        if (const auto* o = d.description.getDynamicObject())
            if (const auto* list = o->getProperty("trackIds").getArray())
                for (const auto& v : *list)
                    ids.push_back(static_cast<std::int64_t>(v));
        if (!ids.empty())
            browser.addTracksToPlaylist(playlistId, ids);
    }
    void itemSelectionChanged(bool isNowSelected) override
    {
        if (isNowSelected)
            browser.itemSelected(itemView, playlistId);
    }
    void itemOpennessChanged(bool) override {}

    LibraryBrowser& browser;
    View itemView;
    juce::String name;
    std::int64_t playlistId;
    bool isFolder;
};

// --- construction ---------------------------------------------------------------------------------------------------

LibraryBrowser::LibraryBrowser(library::Database* db, juce::File artworkFolder, juce::String problem)
    : database(db)
    , artworkDir(std::move(artworkFolder))
    , libraryProblem(std::move(problem))
{
    if (database != nullptr)
    {
        auto opened = database->openReader();
        if (opened)
        {
            reader = std::move(opened).value();
            tableModel = std::make_unique<TrackTableModel>(*reader);
        }
        else
        {
            libraryProblem = juce::String::fromUTF8(opened.error().c_str());
        }
    }

    // Table.
    auto& header = trackTable.getHeader();
    for (const auto& c : kColumns)
    {
        int columnFlags = juce::TableHeaderComponent::defaultFlags;
        if (!c.visible)
            columnFlags &= ~juce::TableHeaderComponent::visible;
        if (c.id == TrackColumn::Colour)
            columnFlags &= ~juce::TableHeaderComponent::resizable;
        header.addColumn(c.name, static_cast<int>(c.id), c.width, 20, 800, columnFlags);
    }
    header.setSortColumnId(static_cast<int>(TrackColumn::Number), true);
    header.setPopupMenuActive(true); // right-click: show / hide columns
    header.setStretchToFitActive(false);
    header.addListener(this);
    header.setColour(juce::TableHeaderComponent::backgroundColourId, kPanel);
    header.setColour(juce::TableHeaderComponent::textColourId, kText);
    header.setColour(juce::TableHeaderComponent::outlineColourId, kBackground);
    trackTable.setRowHeight(kRowHeight);
    trackTable.setMultipleSelectionEnabled(true); // Ctrl / Shift + click; drag several onto a playlist
    trackTable.setWantsKeyboardFocus(false); // keys arrive as actions (KeyboardController)
    trackTable.setColour(juce::ListBox::backgroundColourId, kPanel);
    trackTable.setColour(juce::ListBox::outlineColourId, kBackground);
    trackTable.getViewport()->setScrollBarThickness(10);
    for (auto* bar : {&trackTable.getViewport()->getVerticalScrollBar(),
                      &trackTable.getViewport()->getHorizontalScrollBar(),
                      &treeView.getViewport()->getVerticalScrollBar()})
        bar->setColour(juce::ScrollBar::thumbColourId, kDim.withAlpha(0.6f));

    // Tree.
    treeView.setRootItemVisible(false);
    treeView.setDefaultOpenness(true);
    treeView.setWantsKeyboardFocus(false);
    treeView.setColour(juce::TreeView::backgroundColourId, kPanel);
    treeView.setColour(juce::TreeView::linesColourId, kDim.withAlpha(0.4f));
    treeView.setIndentSize(14);

    // Toolbar.
    for (auto* b : {&importButton, &sizeButton, &loadA, &loadB})
    {
        b->setColour(juce::TextButton::buttonColourId, kPanel);
        b->setColour(juce::TextButton::textColourOffId, kText);
        b->setWantsKeyboardFocus(false);
    }
    importButton.setTooltip("Add a folder of music to the library (or drop folders / files here)");
    importButton.onClick = [this]
    {
        if (onImport)
            onImport({});
    };
    importButton.setEnabled(tableModel != nullptr);
    sizeButton.setTooltip("More room for the browser, over the waveforms (Ctrl+B)");
    sizeButton.onClick = [this]
    {
        if (onToggleSize)
            onToggleSize();
    };
    loadA.onClick = [this] { loadSelected(0); };
    loadB.onClick = [this] { loadSelected(1); };
    loadA.setTooltip("Load the selected track to deck A (Tab)");
    loadB.setTooltip("Load the selected track to deck B (Enter)");
    for (auto* l : {&statusLabel, &countLabel, &activityLabel})
    {
        l->setColour(juce::Label::textColourId, kDim);
        l->setFont(juce::FontOptions(12.0f));
    }
    countLabel.setJustificationType(juce::Justification::centredRight);

    fileBrowser.onLoad = [this](int deck, const juce::File& f)
    {
        if (onLoad)
            onLoad(deck, f);
    };

    // Search and filters.
    for (auto* e : {&search, &bpmFrom, &bpmTo})
    {
        e->setColour(juce::TextEditor::backgroundColourId, kPanel);
        e->setColour(juce::TextEditor::textColourId, kText);
        e->setColour(juce::TextEditor::outlineColourId, kRowAlt);
        e->setColour(juce::TextEditor::focusedOutlineColourId, kAccent);
        e->setFont(juce::FontOptions(13.5f));
        e->setSelectAllWhenFocused(true);
        e->onTextChange = [this] { applyFilter(); };
        e->onReturnKey = [this]
        {
            if (onSearchDone)
                onSearchDone();
        };
        e->onEscapeKey = [this]
        {
            if (onSearchDone)
                onSearchDone();
        };
    }
    search.setTextToShowWhenEmpty("Search title, artist, album, comment (Ctrl+F)", kDim);
    search.setTooltip("Type to filter; Enter or Esc gives the keys back to the decks");
    for (auto* e : {&bpmFrom, &bpmTo})
    {
        e->setInputRestrictions(6, "0123456789.,");
        e->setJustification(juce::Justification::centred);
    }
    bpmFrom.setTextToShowWhenEmpty("min", kDim);
    bpmTo.setTextToShowWhenEmpty("max", kDim);
    bpmLabel.setText("BPM", juce::dontSendNotification);
    bpmDash.setText("-", juce::dontSendNotification);
    for (auto* l : {&bpmLabel, &bpmDash})
    {
        l->setColour(juce::Label::textColourId, kDim);
        l->setFont(juce::FontOptions(12.0f));
        l->setJustificationType(juce::Justification::centred);
    }

    keyBox.addItem("Any key", 1);
    for (int pitch = 0; pitch < 12; ++pitch)
        for (int minor = 0; minor < 2; ++minor)
            keyBox.addItem(TrackTableModel::keyChoiceName(pitch + 12 * minor), 2 + pitch + 12 * minor);
    ratingBox.addItem("Any rating", 1);
    for (int r = 1; r <= 5; ++r)
        ratingBox.addItem(stars(r) + (r < 5 ? " or more" : ""), 1 + r);
    for (auto* box : {&keyBox, &ratingBox, &colourBox, &genreBox, &tagBox})
    {
        box->setColour(juce::ComboBox::backgroundColourId, kPanel);
        box->setColour(juce::ComboBox::textColourId, kText);
        box->setColour(juce::ComboBox::outlineColourId, kRowAlt);
        box->setWantsKeyboardFocus(false);
        box->setSelectedId(1, juce::dontSendNotification);
        box->onChange = [this] { applyFilter(); };
    }
    for (auto* b : {&filterButton, &clearButton})
    {
        b->setColour(juce::TextButton::buttonColourId, kPanel);
        b->setColour(juce::TextButton::buttonOnColourId, kAccent.withAlpha(0.6f));
        b->setColour(juce::TextButton::textColourOffId, kText);
        b->setWantsKeyboardFocus(false);
    }
    filterButton.setClickingTogglesState(false);
    filterButton.setTooltip("Filter by BPM, key, rating, colour, genre and tag");
    filterButton.onClick = [this] { setFiltersShown(!showFilters); };
    clearButton.setTooltip("Clear the search and every filter");
    clearButton.onClick = [this] { clearFilters(); };
    refreshFilterChoices();

    for (auto* c : std::initializer_list<juce::Component*>{&treeView, &trackTable, &importButton, &sizeButton, &loadA,
                                                           &loadB, &statusLabel, &countLabel, &search, &filterButton,
                                                           &activityLabel})
        addAndMakeVisible(c);
    for (auto* c : std::initializer_list<juce::Component*>{&bpmLabel, &bpmFrom, &bpmDash, &bpmTo, &keyBox, &ratingBox,
                                                           &colourBox, &genreBox, &tagBox, &clearButton})
        addChildComponent(c);
    addChildComponent(fileBrowser);

    rebuildTree();
    if (tableModel == nullptr)
    {
        setStatus("Library unavailable" + (libraryProblem.isEmpty() ? juce::String() : ": " + libraryProblem));
        showFolders();
    }
    else
    {
        showCollection();
    }

    if (database != nullptr)
    {
        bridge = std::make_shared<Bridge>();
        bridge->owner = this;
        listenerId = database->addChangeListener(
            [b = bridge](const std::vector<library::Change>& changes)
            {
                bool tracks = false, playlists = false;
                for (const auto& c : changes)
                {
                    // Tags too: smart playlists and the tag filter depend on them.
                    tracks |= c.table == library::Table::Tracks || c.table == library::Table::PlaylistTracks ||
                              c.table == library::Table::TrackTags || c.table == library::Table::Tags;
                    playlists |= c.table == library::Table::Playlists;
                }
                if (!tracks && !playlists)
                    return;
                std::lock_guard lock(b->mutex);
                b->tracks |= tracks;
                b->playlists |= playlists;
                if (b->owner != nullptr)
                    b->owner->triggerAsyncUpdate(); // thread-safe; coalesces a burst of writes
            });
    }
}

LibraryBrowser::~LibraryBrowser()
{
    if (database != nullptr)
        database->removeChangeListener(listenerId);
    if (bridge != nullptr)
    {
        std::lock_guard lock(bridge->mutex);
        bridge->owner = nullptr;
    }
    cancelPendingUpdate();
    stopTimer();
    trackTable.getHeader().removeListener(this);
    treeView.setRootItem(nullptr);
}

// --- tree -----------------------------------------------------------------------------------------------------------

void LibraryBrowser::rebuildTree()
{
    const auto openness = treeView.getOpennessState(true);
    treeView.setRootItem(nullptr);
    treeRoot = std::make_unique<Item>(*this, View::Collection, "root");
    auto* root = treeRoot.get();

    root->addSubItem(new Item(*this, View::Collection, "Collection"));
    root->addSubItem(new Item(*this, View::Missing, "Missing files"));
    auto* playlists = new Item(*this, View::Playlist, "Playlists", 0, true);
    auto* smart = new Item(*this, View::SmartPlaylist, "Smart playlists", 0, true);
    auto* history = new Item(*this, View::History, "History", 0, true);
    root->addSubItem(playlists);
    root->addSubItem(smart);
    root->addSubItem(history);
    root->addSubItem(new Item(*this, View::Folders, "Folders"));

    if (reader != nullptr)
    {
        library::PlaylistRepository repo(*reader);
        std::function<void(juce::TreeViewItem*, std::optional<std::int64_t>)> add =
            [&](juce::TreeViewItem* parent, std::optional<std::int64_t> parentId)
        {
            for (const auto& p : repo.children(parentId))
            {
                const auto label = juce::String::fromUTF8(p.name.c_str());
                switch (p.kind)
                {
                    case library::PlaylistKind::Folder:
                    {
                        auto* folder = new Item(*this, View::Playlist, label, p.id, true);
                        parent->addSubItem(folder);
                        add(folder, p.id);
                        break;
                    }
                    case library::PlaylistKind::Playlist:
                        parent->addSubItem(new Item(*this, View::Playlist, label, p.id));
                        break;
                    case library::PlaylistKind::Smart:
                        smart->addSubItem(new Item(*this, View::SmartPlaylist, label, p.id));
                        break;
                    case library::PlaylistKind::History:
                        history->addSubItem(new Item(*this, View::History, label, p.id));
                        break;
                }
            }
        };
        add(playlists, std::nullopt);
    }
    treeView.setRootItem(root);
    if (openness != nullptr)
        treeView.restoreOpennessState(*openness, false);

    // A playlist just created from a menu: show it.
    if (selectAfterRebuild != 0)
    {
        const auto id = std::exchange(selectAfterRebuild, 0);
        currentView = View::Playlist;
        currentPlaylist = id;
        for (int i = 0; i < treeView.getNumRowsInTree(); ++i)
            if (auto* item = dynamic_cast<Item*>(treeView.getItemOnRow(i)); item != nullptr && item->playlistId == id)
            {
                item->setSelected(true, true, juce::dontSendNotification);
                treeView.scrollToKeepItemVisible(item);
                setView(item->itemView, id); // a playlist or a smart playlist
                return;
            }
    }

    // Re-select what was shown.
    for (int i = 0; i < treeView.getNumRowsInTree(); ++i)
        if (auto* item = dynamic_cast<Item*>(treeView.getItemOnRow(i));
            item != nullptr && !item->isFolder && item->itemView == currentView &&
            (currentView == View::Collection || currentView == View::Folders || currentView == View::Missing ||
             item->playlistId == currentPlaylist))
        {
            item->setSelected(true, true, juce::dontSendNotification);
            break;
        }
}

void LibraryBrowser::itemSelected(View v, std::int64_t playlistId)
{
    setView(v, playlistId);
}

void LibraryBrowser::setView(View v, std::int64_t playlistId)
{
    if (tableModel == nullptr && v != View::Folders)
        v = View::Folders;
    currentView = v;
    currentPlaylist = playlistId;
    const bool files = v == View::Folders;
    fileBrowser.setVisible(files);
    trackTable.setVisible(!files);
    loadA.setVisible(!files);
    loadB.setVisible(!files);
    search.setVisible(!files);
    filterButton.setVisible(!files);
    setFiltersShown(showFilters); // the filter row only over the track table
    if (!files && tableModel != nullptr)
    {
        // Smart playlists show what their rules match; history (P2-23) has no stored tracks yet.
        TrackSource s;
        if (v == View::Playlist || v == View::History)
            s = {TrackSource::Kind::Playlist, playlistId};
        else if (v == View::SmartPlaylist)
            s = {playlistId > 0 ? TrackSource::Kind::Smart : TrackSource::Kind::Playlist, playlistId};
        else if (v == View::Missing)
            s = {TrackSource::Kind::Missing, 0};
        tableModel->setSource(s);
        trackTable.updateContent();
        trackTable.deselectAllRows();
        // Back to the top. (Not scrollToEnsureRowIsOnscreen: before the first layout the table has no height and
        // that would scroll down by a row.)
        trackTable.getViewport()->setViewPosition(0, 0);
    }
    updateArtwork();
    updateCount();
    resized();
    repaint();
}

void LibraryBrowser::showCollection()
{
    for (int i = 0; i < treeView.getNumRowsInTree(); ++i)
        if (auto* item = dynamic_cast<Item*>(treeView.getItemOnRow(i)); item != nullptr && item->itemView == View::Collection)
        {
            item->setSelected(true, true, juce::dontSendNotification);
            break;
        }
    setView(View::Collection, 0);
}

void LibraryBrowser::showPlaylist(std::int64_t playlistId)
{
    for (int i = 0; i < treeView.getNumRowsInTree(); ++i)
        if (auto* item = dynamic_cast<Item*>(treeView.getItemOnRow(i));
            item != nullptr && item->playlistId == playlistId && !item->isFolder)
        {
            item->setSelected(true, true, juce::dontSendNotification);
            setView(item->itemView, playlistId);
            return;
        }
}

void LibraryBrowser::showFolders()
{
    for (int i = 0; i < treeView.getNumRowsInTree(); ++i)
        if (auto* item = dynamic_cast<Item*>(treeView.getItemOnRow(i)); item != nullptr && item->itemView == View::Folders)
        {
            item->setSelected(true, true, juce::dontSendNotification);
            break;
        }
    setView(View::Folders, 0);
}

// --- table ----------------------------------------------------------------------------------------------------------

int LibraryBrowser::getNumRows()
{
    return tableModel != nullptr && currentView != View::Folders ? tableModel->size() : 0;
}

void LibraryBrowser::paintRowBackground(juce::Graphics& g, int row, int width, int height, bool selected)
{
    if (selected)
        g.setColour(kAccent.withAlpha(treeFocused ? 0.3f : 0.55f));
    else
        g.setColour(row % 2 == 0 ? kPanel : kRowAlt);
    g.fillRect(0, 0, width, height);
}

void LibraryBrowser::paintCell(juce::Graphics& g, int row, int column, int width, int height, bool selected)
{
    const auto* r = tableModel != nullptr ? tableModel->row(row) : nullptr;
    if (r == nullptr)
        return;
    const auto col = static_cast<TrackColumn>(column);
    if (col == TrackColumn::Colour)
    {
        if (r->colour)
        {
            g.setColour(juce::Colour(0xff000000u | *r->colour));
            g.fillRoundedRectangle(juce::Rectangle<float>(6.0f, 5.0f, 12.0f, static_cast<float>(height) - 10.0f), 2.0f);
        }
        return;
    }
    juce::String text;
    auto justification = juce::Justification::centredLeft;
    switch (col)
    {
        case TrackColumn::Number: text = juce::String(row + 1); justification = juce::Justification::centredRight; break;
        case TrackColumn::Title: text = r->title; break;
        case TrackColumn::Artist: text = r->artist; break;
        case TrackColumn::Album: text = r->album; break;
        case TrackColumn::Bpm:
            text = r->bpm > 0.0 ? juce::String(r->bpm, 2) : juce::String();
            justification = juce::Justification::centredRight;
            break;
        case TrackColumn::Key: text = r->key; break;
        case TrackColumn::Time:
            text = TrackTableModel::timeText(r->seconds);
            justification = juce::Justification::centredRight;
            break;
        case TrackColumn::Rating:
        {
            // Five stars in fixed slots, so a click can tell which one it hit (starAt).
            g.setFont(juce::FontOptions(13.5f));
            for (int i = 1; i <= 5; ++i)
            {
                g.setColour(i <= r->rating ? juce::Colour(0xffe8c832) : kDim.withAlpha(0.45f));
                g.drawText(juce::String::fromUTF8(i <= r->rating ? "\xE2\x98\x85" : "\xE2\x98\x86"),
                           kStarLeft + (i - 1) * kStarWidth, 0, kStarWidth, height, juce::Justification::centred,
                           false);
            }
            return;
        }
        case TrackColumn::Tags: text = r->tags; break;
        case TrackColumn::Mix: // a saved transition into the next row's track (P4-19)
            if (transitionLabel)
                if (const auto* next = tableModel->row(row + 1))
                    if (const auto label = transitionLabel(r->id, next->id); label.isNotEmpty())
                        text = juce::String::fromUTF8("\xE2\x86\x92 ") + label;
            break;
        case TrackColumn::Genre: text = r->genre; break;
        case TrackColumn::DateAdded: text = dateText(r->dateAdded); break;
        case TrackColumn::Comment: text = r->comment; break;
        case TrackColumn::Colour: break;
    }
    // A missing file: the row in red (its file isn't where the library says).
    g.setColour(r->missing ? juce::Colour(0xffe07a70).withAlpha(selected ? 1.0f : 0.85f)
                : selected ? juce::Colours::white
                           : (col == TrackColumn::Number ? kDim : kText));
    g.setFont(juce::FontOptions(13.5f));
    g.drawText(text, 5, 0, width - 10, height, justification, true);
}

void LibraryBrowser::sortOrderChanged(int newSortColumnId, bool isForwards)
{
    if (tableModel == nullptr)
        return;
    const auto keep = selectedTrackId();
    tableModel->setSort(static_cast<TrackColumn>(newSortColumnId), isForwards);
    trackTable.updateContent();
    if (keep != 0)
        selectTrack(keep);
    trackTable.repaint();
}

void LibraryBrowser::selectedRowsChanged(int)
{
    updateArtwork();
}

void LibraryBrowser::cellClicked(int row, int column, const juce::MouseEvent& e)
{
    treeFocused = false;
    treeView.repaint();
    // Inline edits (P2-08): the stars rate, the colour cell opens the palette.
    if (!e.mods.isPopupMenu() && !e.mods.isCommandDown() && !e.mods.isShiftDown() && tableModel != nullptr)
    {
        const auto col = static_cast<TrackColumn>(column);
        if (col == TrackColumn::Rating || col == TrackColumn::Colour)
        {
            const auto& header = trackTable.getHeader();
            const int columnX = header.getColumnPosition(header.getIndexOfColumnId(column, true)).getX();
            const int x = e.getEventRelativeTo(&trackTable).x + trackTable.getViewport()->getViewPositionX() - columnX;
            if (!trackTable.isRowSelected(row))
                trackTable.selectRow(row);
            if (col == TrackColumn::Rating)
            {
                if (const int star = starAt(x); star > 0)
                {
                    const auto* r = tableModel->row(row);
                    rateSelected(r != nullptr && r->rating == star ? 0 : star); // the same star again clears
                }
            }
            else
            {
                colourMenu().showMenuAsync(juce::PopupMenu::Options().withMousePosition());
            }
            return;
        }
    }
    if (e.mods.isPopupMenu())
    {
        if (!trackTable.isRowSelected(row))
            trackTable.selectRow(row);
        menuForTracks().showMenuAsync(juce::PopupMenu::Options().withMousePosition());
    }
}

void LibraryBrowser::cellDoubleClicked(int row, int, const juce::MouseEvent&)
{
    trackTable.selectRow(row);
    loadSelected(0);
}

juce::var LibraryBrowser::getDragSourceDescription(const juce::SparseSet<int>& rows)
{
    // {path: the first track's file (a deck loads it), trackIds: every dragged track (a playlist adds them)}.
    if (rows.isEmpty() || tableModel == nullptr)
        return {};
    const auto* first = tableModel->row(rows[0]);
    if (first == nullptr)
        return {};
    auto* o = new juce::DynamicObject();
    o->setProperty("path", first->path);
    juce::Array<juce::var> ids;
    for (int i = 0; i < rows.size(); ++i)
        ids.add(static_cast<juce::int64>(tableModel->trackIdAt(rows[i])));
    o->setProperty("trackIds", ids);
    return juce::var(o);
}

juce::String LibraryBrowser::getCellTooltip(int row, int)
{
    const auto* r = tableModel != nullptr ? tableModel->row(row) : nullptr;
    return r != nullptr ? r->path : juce::String();
}

std::int64_t LibraryBrowser::selectedTrackId() const
{
    if (tableModel == nullptr || currentView == View::Folders)
        return 0;
    return tableModel->trackIdAt(trackTable.getSelectedRow());
}

juce::File LibraryBrowser::selectedTrackFile() const
{
    if (currentView == View::Folders)
        return fileBrowser.selectedFile();
    if (tableModel == nullptr)
        return {};
    const auto* r = tableModel->row(trackTable.getSelectedRow());
    if (r == nullptr || !juce::File::isAbsolutePath(r->path))
        return {};
    return juce::File(r->path);
}

void LibraryBrowser::selectTrack(std::int64_t trackId)
{
    if (tableModel == nullptr)
        return;
    const int row = tableModel->rowOfTrack(trackId);
    if (row >= 0)
    {
        trackTable.selectRow(row);
        trackTable.scrollToEnsureRowIsOnscreen(row);
    }
}

// --- layout persistence -----------------------------------------------------------------------------------------------

juce::String LibraryBrowser::columnLayout() const
{
    return trackTable.getHeader().toString();
}

void LibraryBrowser::setColumnLayout(const juce::String& layout)
{
    if (layout.isEmpty())
        return;
    const juce::ScopedValueSetter<bool> quiet(restoringLayout, true);
    auto& header = trackTable.getHeader();
    header.restoreFromString(layout);
    // The sort is part of the string: apply it to the model too.
    if (tableModel != nullptr && header.getSortColumnId() > 0)
        tableModel->setSort(static_cast<TrackColumn>(header.getSortColumnId()), header.isSortedForwards());
    trackTable.updateContent();
}

void LibraryBrowser::syncWithHeader()
{
    auto& header = trackTable.getHeader();
    if (tableModel != nullptr && header.getSortColumnId() > 0)
        sortOrderChanged(header.getSortColumnId(), header.isSortedForwards());
    layoutChanged();
}

juce::StringArray LibraryBrowser::treeRowNames() const
{
    juce::StringArray names;
    for (int i = 0; i < treeView.getNumRowsInTree(); ++i)
        if (const auto* item = dynamic_cast<const Item*>(treeView.getItemOnRow(i)))
            names.add(item->name);
    return names;
}

void LibraryBrowser::layoutChanged()
{
    if (!restoringLayout && onColumnLayoutChanged)
        onColumnLayoutChanged(columnLayout());
}

void LibraryBrowser::tableColumnsChanged(juce::TableHeaderComponent*)
{
    layoutChanged();
}

void LibraryBrowser::tableColumnsResized(juce::TableHeaderComponent*)
{
    layoutChanged();
}

void LibraryBrowser::tableSortOrderChanged(juce::TableHeaderComponent*)
{
    layoutChanged();
}

// --- keyboard -----------------------------------------------------------------------------------------------------------

int LibraryBrowser::pageSize() const
{
    if (currentView == View::Folders)
        return fileBrowser.pageSize();
    return std::max(1, trackTable.getHeight() / kRowHeight - 2);
}

void LibraryBrowser::moveSelection(int delta)
{
    if (treeFocused)
    {
        // Move through the tree's selectable items.
        auto* selected = treeView.getSelectedItem(0);
        int row = selected != nullptr ? selected->getRowNumberInTree() : -1;
        const int rows = treeView.getNumRowsInTree();
        const int step = delta > 0 ? 1 : -1;
        for (int moved = 0; moved < std::abs(delta);)
        {
            const int next = row + step;
            if (next < 0 || next >= rows)
                break;
            row = next;
            if (auto* item = treeView.getItemOnRow(row); item != nullptr && item->canBeSelected())
                ++moved;
        }
        if (auto* item = treeView.getItemOnRow(row); item != nullptr && item->canBeSelected())
        {
            item->setSelected(true, true);
            treeView.scrollToKeepItemVisible(item);
        }
        return;
    }
    if (currentView == View::Folders)
    {
        fileBrowser.moveSelection(delta);
        return;
    }
    const int n = getNumRows();
    if (n == 0)
        return;
    const int current = trackTable.getSelectedRow();
    const int row = std::clamp(current < 0 ? (delta > 0 ? 0 : n - 1) : current + delta, 0, n - 1);
    trackTable.selectRow(row);
    trackTable.scrollToEnsureRowIsOnscreen(row);
}

bool LibraryBrowser::loadSelected(int deck)
{
    if (currentView == View::Folders)
        return fileBrowser.loadSelected(deck);
    const auto file = selectedTrackFile();
    if (!file.existsAsFile())
    {
        if (const auto id = selectedTrackId(); id != 0)
        {
            setStatus("File not found: " + file.getFullPathName() + " (right-click > Relocate to find it)");
            if (database != nullptr)
                library::setTrackMissing(*database, id, true); // shows up in Missing files
        }
        return selectedTrackId() != 0;
    }
    if (onLoad)
        onLoad(deck, file);
    return true;
}

void LibraryBrowser::collapse()
{
    if (!treeFocused)
    {
        if (currentView == View::Folders && fileBrowser.isVisible())
        {
            fileBrowser.goToParentFolder();
            return;
        }
        treeFocused = true; // from the table to the tree
        treeView.repaint();
        trackTable.repaint();
        return;
    }
    if (auto* item = treeView.getSelectedItem(0))
    {
        if (auto* parent = item->getParentItem(); parent != nullptr && parent != treeRoot.get())
        {
            parent->setOpen(false);
            // The folder itself can't be selected: pick the first selectable item around it.
            treeView.scrollToKeepItemVisible(parent);
        }
    }
}

void LibraryBrowser::expand()
{
    if (!treeFocused)
        return;
    if (auto* item = treeView.getSelectedItem(0))
    {
        if (auto* parent = item->getParentItem(); parent != nullptr && !parent->isOpen())
        {
            parent->setOpen(true);
            return;
        }
    }
    treeFocused = false; // back to the table
    treeView.repaint();
    trackTable.repaint();
}

// --- library changes --------------------------------------------------------------------------------------------------

void LibraryBrowser::handleAsyncUpdate()
{
    applyLibraryChanges(false);
}

void LibraryBrowser::timerCallback()
{
    stopTimer();
    applyLibraryChanges(true);
}

void LibraryBrowser::applyLibraryChanges(bool now)
{
    constexpr double kMinIntervalMs = 400.0;
    const double t = juce::Time::getMillisecondCounterHiRes();
    if (!now && t - lastRefreshMs < kMinIntervalMs)
    {
        if (!isTimerRunning())
            startTimer(static_cast<int>(kMinIntervalMs - (t - lastRefreshMs)) + 1);
        return; // the changes stay noted in the bridge until then
    }
    stopTimer();
    lastRefreshMs = t;
    bool tracks = false, playlists = false;
    if (bridge != nullptr)
    {
        std::lock_guard lock(bridge->mutex);
        tracks = std::exchange(bridge->tracks, false);
        playlists = std::exchange(bridge->playlists, false);
    }
    refreshFromLibrary(tracks, playlists);
}

void LibraryBrowser::refreshFromLibrary(bool tracks, bool playlists)
{
    if (playlists)
        rebuildTree();
    if (tracks)
        refreshFilterChoices();
    if ((tracks || playlists) && tableModel != nullptr)
    {
        const auto keep = selectedTrackIds(); // every selected track, not just one (P2-08: edits on several)
        tableModel->reload();
        trackTable.updateContent();
        if (!selectRowsAfterReload.empty())
        {
            trackTable.deselectAllRows();
            for (const int r : std::exchange(selectRowsAfterReload, {}))
                trackTable.selectRow(r, false, false);
        }
        else if (!keep.empty())
        {
            trackTable.deselectAllRows();
            bool first = true;
            for (const auto id : keep)
                if (const int row = tableModel->rowOfTrack(id); row >= 0)
                {
                    trackTable.selectRow(row, !first, false); // scroll to the first one only
                    first = false;
                }
        }
        trackTable.repaint();
        updateCount();
    }
}

void LibraryBrowser::setKeyNotation(util::KeyNotation notation)
{
    if (tableModel == nullptr || tableModel->keyNotation() == notation)
        return;
    tableModel->setKeyNotation(notation);
    trackTable.updateContent();
    trackTable.repaint();
}

void LibraryBrowser::setBig(bool big)
{
    sizeButton.setButtonText(big ? "SMALLER" : "BIGGER");
}

void LibraryBrowser::setActivity(const juce::String& text)
{
    if (text == activityLabel.getText())
        return;
    const bool layout = text.isEmpty() != activityLabel.getText().isEmpty();
    activityLabel.setText(text, juce::dontSendNotification);
    if (layout)
        resized();
}

void LibraryBrowser::repaintRows()
{
    trackTable.repaint();
}

void LibraryBrowser::setStatus(const juce::String& text)
{
    statusLabel.setText(text, juce::dontSendNotification);
}

void LibraryBrowser::updateCount()
{
    if (currentView == View::Folders || tableModel == nullptr)
    {
        countLabel.setText({}, juce::dontSendNotification);
        return;
    }
    const int n = tableModel->size();
    juce::String text;
    if (!tableModel->filter().isEmpty())
        text << n << " of " << tableModel->unfilteredSize() << " tracks";
    else
        text << n << (n == 1 ? " track" : " tracks");
    countLabel.setText(text, juce::dontSendNotification);
    filterButton.setToggleState(showFilters || !filterFromControls().isEmpty(), juce::dontSendNotification);
}

// --- search and filters -------------------------------------------------------------------------------------------------

juce::String LibraryBrowser::colourName(std::uint32_t rgb)
{
    if (const juce::String named = paletteName(rgb); named.isNotEmpty())
        return named.toLowerCase();
    const auto c = juce::Colour(0xff000000u | rgb);
    if (c.getSaturation() < 0.15f)
        return c.getBrightness() > 0.6f ? "white" : c.getBrightness() < 0.25f ? "black" : "grey";
    static const std::pair<float, const char*> hues[] = {{0.04f, "red"},    {0.11f, "orange"}, {0.19f, "yellow"},
                                                         {0.45f, "green"},  {0.55f, "aqua"},   {0.70f, "blue"},
                                                         {0.82f, "purple"}, {0.95f, "pink"},   {1.01f, "red"}};
    const float h = c.getHue();
    for (const auto& [limit, name] : hues)
        if (h < limit)
            return name;
    return "red";
}

void LibraryBrowser::refreshFilterChoices()
{
    if (reader == nullptr)
        return;
    const juce::ScopedValueSetter<bool> quiet(settingControls, true);
    const auto keepGenre = genreBox.getSelectedId() > 1 ? genreBox.getText() : juce::String();
    const auto keepColour = colourBox.getSelectedId() > 1 ? colourChoices[static_cast<std::size_t>(colourBox.getSelectedId() - 2)]
                                                          : 0xFFFFFFFFu;
    const auto keepTag = tagBox.getSelectedId() > 1 ? tagChoices[static_cast<std::size_t>(tagBox.getSelectedId() - 2)] : 0;

    genreBox.clear(juce::dontSendNotification);
    genreBox.addItem("Any genre", 1);
    if (auto* s = reader->prepare("SELECT DISTINCT genre FROM tracks WHERE genre IS NOT NULL ORDER BY genre COLLATE NOCASE"))
    {
        int id = 2;
        while (s->step())
        {
            const auto g = juce::String::fromUTF8(s->getText(0).c_str());
            genreBox.addItem(g, id);
            if (g == keepGenre)
                genreBox.setSelectedId(id, juce::dontSendNotification);
            ++id;
        }
    }
    if (genreBox.getSelectedId() == 0)
        genreBox.setSelectedId(1, juce::dontSendNotification);

    colourBox.clear(juce::dontSendNotification);
    colourChoices.clear();
    colourBox.addItem("Any colour", 1);
    if (auto* s = reader->prepare("SELECT DISTINCT color FROM tracks WHERE color IS NOT NULL ORDER BY color"))
        while (s->step())
        {
            const auto rgb = static_cast<std::uint32_t>(s->getInt(0));
            colourChoices.push_back(rgb);
            const int id = static_cast<int>(colourChoices.size()) + 1;
            colourBox.addItem(juce::String(paletteName(rgb)).isNotEmpty()
                                  ? juce::String(paletteName(rgb))
                                  : colourName(rgb) + juce::String::formatted(" (#%06X)", static_cast<unsigned>(rgb)),
                              id);
            if (rgb == keepColour)
                colourBox.setSelectedId(id, juce::dontSendNotification);
        }
    if (colourBox.getSelectedId() == 0)
        colourBox.setSelectedId(1, juce::dontSendNotification);

    tagBox.clear(juce::dontSendNotification);
    tagChoices.clear();
    tagBox.addItem("Any tag", 1);
    library::TagRepository tags(*reader);
    std::unordered_map<std::int64_t, juce::String> categoryNames;
    for (const auto& c : tags.categories())
        categoryNames[c.id] = juce::String::fromUTF8(c.name.c_str());
    for (const auto& t : tags.tags())
    {
        tagChoices.push_back(t.id);
        const int id = static_cast<int>(tagChoices.size()) + 1;
        const auto name = juce::String::fromUTF8(t.name.c_str());
        tagBox.addItem(t.categoryId ? categoryNames[*t.categoryId] + ": " + name : name, id);
        if (t.id == keepTag)
            tagBox.setSelectedId(id, juce::dontSendNotification);
    }
    if (tagBox.getSelectedId() == 0)
        tagBox.setSelectedId(1, juce::dontSendNotification);
}

TrackFilter LibraryBrowser::filterFromControls() const
{
    TrackFilter f;
    f.text = search.getText().trim();
    const auto bpm = [](const juce::TextEditor& e) -> std::optional<double>
    {
        const auto t = e.getText().replaceCharacter(',', '.').trim();
        if (t.isEmpty())
            return std::nullopt;
        const double v = t.getDoubleValue();
        return v > 0.0 ? std::optional<double>{v} : std::nullopt;
    };
    f.bpmMin = bpm(bpmFrom);
    f.bpmMax = bpm(bpmTo);
    if (f.bpmMin && f.bpmMax && *f.bpmMin > *f.bpmMax)
        std::swap(f.bpmMin, f.bpmMax);
    if (keyBox.getSelectedId() > 1)
        f.keys.push_back(keyBox.getSelectedId() - 2);
    if (ratingBox.getSelectedId() > 1)
        f.minRating = ratingBox.getSelectedId() - 1;
    if (const int c = colourBox.getSelectedId(); c > 1 && c - 2 < static_cast<int>(colourChoices.size()))
        f.colours.push_back(colourChoices[static_cast<std::size_t>(c - 2)]);
    if (genreBox.getSelectedId() > 1)
        f.genre = genreBox.getText();
    if (const int t = tagBox.getSelectedId(); t > 1 && t - 2 < static_cast<int>(tagChoices.size()))
        f.tagIds.push_back(tagChoices[static_cast<std::size_t>(t - 2)]);
    return f;
}

void LibraryBrowser::applyFilter()
{
    if (settingControls || tableModel == nullptr)
        return;
    const auto keep = selectedTrackId();
    tableModel->setFilter(filterFromControls());
    trackTable.updateContent();
    trackTable.deselectAllRows();
    if (keep != 0)
        selectTrack(keep);
    if (trackTable.getSelectedRow() < 0)
        trackTable.getViewport()->setViewPosition(0, 0);
    trackTable.repaint();
    updateCount();
    repaint();
}

void LibraryBrowser::setFilter(const TrackFilter& f)
{
    {
        const juce::ScopedValueSetter<bool> quiet(settingControls, true);
        search.setText(f.text, juce::dontSendNotification);
        bpmFrom.setText(f.bpmMin ? juce::String(*f.bpmMin) : juce::String(), juce::dontSendNotification);
        bpmTo.setText(f.bpmMax ? juce::String(*f.bpmMax) : juce::String(), juce::dontSendNotification);
        keyBox.setSelectedId(f.keys.empty() ? 1 : 2 + f.keys.front(), juce::dontSendNotification);
        ratingBox.setSelectedId(1 + std::clamp(f.minRating, 0, 5), juce::dontSendNotification);
        colourBox.setSelectedId(1, juce::dontSendNotification);
        for (std::size_t i = 0; i < colourChoices.size(); ++i)
            if (!f.colours.empty() && colourChoices[i] == f.colours.front())
                colourBox.setSelectedId(static_cast<int>(i) + 2, juce::dontSendNotification);
        genreBox.setSelectedId(1, juce::dontSendNotification);
        for (int i = 0; i < genreBox.getNumItems(); ++i)
            if (f.genre.isNotEmpty() && genreBox.getItemText(i).equalsIgnoreCase(f.genre))
                genreBox.setSelectedItemIndex(i, juce::dontSendNotification);
        tagBox.setSelectedId(1, juce::dontSendNotification);
        for (std::size_t i = 0; i < tagChoices.size(); ++i)
            if (!f.tagIds.empty() && tagChoices[i] == f.tagIds.front())
                tagBox.setSelectedId(static_cast<int>(i) + 2, juce::dontSendNotification);
    }
    applyFilter();
}

void LibraryBrowser::clearFilters()
{
    setFilter({});
}

void LibraryBrowser::setFiltersShown(bool shown)
{
    showFilters = shown;
    for (auto* c : std::initializer_list<juce::Component*>{&bpmLabel, &bpmFrom, &bpmDash, &bpmTo, &keyBox, &ratingBox,
                                                           &colourBox, &genreBox, &tagBox, &clearButton})
        c->setVisible(shown && currentView != View::Folders);
    updateCount();
    resized();
}

void LibraryBrowser::focusSearch()
{
    if (currentView == View::Folders || tableModel == nullptr)
        showCollection();
    search.grabKeyboardFocus();
    search.selectAll();
}

// --- artwork --------------------------------------------------------------------------------------------------------------

void LibraryBrowser::updateArtwork()
{
    artwork = {};
    const auto id = selectedTrackId();
    if (id != 0 && reader != nullptr && artworkDir != juce::File())
    {
        auto* s = reader->prepare("SELECT path FROM analysis_artifacts WHERE track_id = ? AND kind = 'artwork'");
        if (s != nullptr && s->bind(1, id).step())
        {
            const auto file = artworkDir.getChildFile(juce::String::fromUTF8(s->getText(0).c_str()));
            s->reset();
            if (file.existsAsFile())
                artwork = juce::ImageFileFormat::loadFrom(file);
        }
        else if (s != nullptr)
        {
            s->reset();
        }
    }
    repaint(artworkArea);
}

// --- drops from Explorer: import ------------------------------------------------------------------------------------------

bool LibraryBrowser::isInterestedInFileDrag(const juce::StringArray& files)
{
    if (tableModel == nullptr || !onImport)
        return false;
    for (const auto& path : files)
        if (const juce::File f(path); f.isDirectory() || engine::isSupportedTrackFile(f) ||
                                      f.hasFileExtension(".m3u;.m3u8;.xml"))
            return true;
    return false;
}

void LibraryBrowser::fileDragEnter(const juce::StringArray&, int, int)
{
    dropHover = true;
    repaint();
}

void LibraryBrowser::fileDragExit(const juce::StringArray&)
{
    dropHover = false;
    repaint();
}

void LibraryBrowser::filesDropped(const juce::StringArray& files, int, int)
{
    dropHover = false;
    repaint();
    juce::Array<juce::File> toImport;
    for (const auto& path : files)
        if (const juce::File f(path); f.isDirectory() || engine::isSupportedTrackFile(f) ||
                                      f.hasFileExtension(".m3u;.m3u8;.xml"))
            toImport.add(f); // playlist files become playlists, library XML a library import
    if (!toImport.isEmpty() && onImport)
        onImport(toImport);
}

// --- layout / paint ---------------------------------------------------------------------------------------------------------

void LibraryBrowser::resized()
{
    auto area = getLocalBounds().reduced(8);
    auto top = area.removeFromTop(kToolbarHeight);
    importButton.setBounds(top.removeFromLeft(90));
    top.removeFromLeft(kTreeWidth - 90 + 6);
    loadB.setBounds(top.removeFromRight(72));
    top.removeFromRight(6);
    loadA.setBounds(top.removeFromRight(72));
    top.removeFromRight(10);
    sizeButton.setBounds(top.removeFromRight(84));
    top.removeFromRight(10);
    countLabel.setBounds(top.removeFromRight(130));
    activityLabel.setBounds(top.removeFromRight(activityLabel.getText().isEmpty() ? 0 : 230));
    search.setBounds(top.removeFromLeft(std::clamp(top.getWidth() / 3, 160, 340)));
    top.removeFromLeft(6);
    filterButton.setBounds(top.removeFromLeft(70));
    top.removeFromLeft(10);
    statusLabel.setBounds(top);
    area.removeFromTop(6);

    // The filter row, above the table (not over the tree).
    const bool filterRow = showFilters && currentView != View::Folders;
    if (filterRow)
    {
        auto row = area.withTrimmedLeft(kTreeWidth + 6).removeFromTop(kFilterRowHeight);
        bpmLabel.setBounds(row.removeFromLeft(36));
        bpmFrom.setBounds(row.removeFromLeft(52));
        bpmDash.setBounds(row.removeFromLeft(14));
        bpmTo.setBounds(row.removeFromLeft(52));
        row.removeFromLeft(8);
        clearButton.setBounds(row.removeFromRight(64));
        row.removeFromRight(8);
        const int w = std::max(60, row.getWidth() / 5 - 6);
        for (auto* box : {&keyBox, &ratingBox, &colourBox, &genreBox, &tagBox})
        {
            box->setBounds(row.removeFromLeft(w));
            row.removeFromLeft(6);
        }
    }

    auto left = area.removeFromLeft(kTreeWidth);
    area.removeFromLeft(6);
    // Cover art under the tree when there is room for it (the tree keeps at least two thirds).
    const int art = std::min({left.getWidth(), left.getHeight() / 3, 120});
    if (art >= 64)
    {
        artworkArea = left.removeFromBottom(art).withSizeKeepingCentre(art, art);
        left.removeFromBottom(6);
    }
    else
    {
        artworkArea = {};
    }
    treeView.setBounds(left);
    const auto tableArea = filterRow ? area.withTrimmedTop(kFilterRowHeight + 6) : area;
    trackTable.setBounds(tableArea);
    fileBrowser.setBounds(area);
}

void LibraryBrowser::paint(juce::Graphics& g)
{
    g.setColour(kBackground);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
    g.setColour(kAccent.withAlpha(0.8f));
    g.fillRect(getLocalBounds().removeFromTop(3).reduced(8, 0).toFloat());

    if (!artworkArea.isEmpty())
    {
        g.setColour(kPanel);
        g.fillRect(artworkArea.toFloat());
        if (artwork.isValid())
            g.drawImage(artwork, artworkArea.toFloat(), juce::RectanglePlacement::centred);
        else
        {
            g.setColour(kDim.withAlpha(0.5f));
            g.setFont(juce::FontOptions(12.0f));
            g.drawText(selectedTrackId() != 0 ? "no artwork" : "", artworkArea, juce::Justification::centred);
        }
    }

}

void LibraryBrowser::paintOverChildren(juce::Graphics& g)
{
    if (dropHover)
    {
        g.setColour(kAccent.withAlpha(0.18f));
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
        g.setColour(kAccent);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.5f), 8.0f, 3.0f);
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(20.0f, juce::Font::bold));
        g.drawText("Drop to add to the library", getLocalBounds(), juce::Justification::centred);
        return;
    }
    if (tableModel != nullptr && currentView != View::Folders && tableModel->size() == 0)
    {
        g.setColour(kDim);
        g.setFont(juce::FontOptions(15.0f));
        const auto where = trackTable.getBounds().withTrimmedTop(kRowHeight + 4);
        const juce::String text = currentView == View::Collection
                                      ? "No tracks yet. Press IMPORT or drop a music folder here."
                                  : currentView == View::SmartPlaylist ? "No tracks match this smart playlist's rules (yet)."
                                  : currentView == View::History       ? "Played tracks will appear here."
                                  : currentView == View::Missing       ? "No missing files."
                                                                       : "Empty playlist.";
        g.drawFittedText(text, where.reduced(20), juce::Justification::centredTop, 2);
    }
}

// --- playlists (P2-06) -----------------------------------------------------------------------------------------------

std::optional<library::Playlist> LibraryBrowser::playlistInfo(std::int64_t id)
{
    if (reader == nullptr || id <= 0)
        return std::nullopt;
    return library::PlaylistRepository(*reader).get(id);
}

std::int64_t LibraryBrowser::shownPlaylist() const noexcept
{
    return currentView == View::Playlist ? currentPlaylist : 0;
}

bool LibraryBrowser::canReorder() const noexcept
{
    return shownPlaylist() > 0 && tableModel != nullptr && tableModel->sortColumn() == TrackColumn::Number &&
           tableModel->sortAscending() && tableModel->filter().isEmpty();
}

std::vector<int> LibraryBrowser::selectedRows() const
{
    std::vector<int> rows;
    const auto set = trackTable.getSelectedRows();
    for (int i = 0; i < set.size(); ++i)
        rows.push_back(set[i]);
    return rows;
}

std::vector<std::int64_t> LibraryBrowser::selectedTrackIds() const
{
    std::vector<std::int64_t> ids;
    if (tableModel == nullptr || currentView == View::Folders)
        return ids;
    for (const int r : selectedRows())
        ids.push_back(tableModel->trackIdAt(r));
    return ids;
}

juce::String LibraryBrowser::uniquePlaylistName(const juce::String& base, std::optional<std::int64_t> parent)
{
    juce::StringArray taken;
    if (reader != nullptr)
        for (const auto& p : library::PlaylistRepository(*reader).children(parent))
            taken.add(juce::String::fromUTF8(p.name.c_str()));
    if (!taken.contains(base, true))
        return base;
    for (int n = 2;; ++n)
        if (const auto name = base + " " + juce::String(n); !taken.contains(name, true))
            return name;
}

void LibraryBrowser::createPlaylist(const juce::String& name, std::optional<std::int64_t> parent, bool folder)
{
    if (database == nullptr)
        return;
    const auto text = name.trim().toStdString();
    juce::Component::SafePointer<LibraryBrowser> self(this);
    database->write(
        [text, parent, folder, self](library::Connection& c)
        {
            library::Playlist p;
            p.parentId = parent;
            p.name = text;
            p.kind = folder ? library::PlaylistKind::Folder : library::PlaylistKind::Playlist;
            const auto created = library::PlaylistRepository(c).create(p);
            const auto error = created ? juce::String() : juce::String::fromUTF8(created.error().c_str());
            const auto id = created ? created.value() : std::int64_t{0};
            // Before the change notification's refresh: select the new playlist once the tree has it.
            juce::MessageManager::callAsync(
                [self, id, error, folder]
                {
                    if (self == nullptr)
                        return;
                    if (error.isNotEmpty())
                        self->setStatus("Can't create it: " + error);
                    else if (!folder)
                        self->selectAfterRebuild = id;
                });
        });
}

void LibraryBrowser::renamePlaylist(std::int64_t id, const juce::String& name)
{
    if (database == nullptr)
        return;
    const auto text = name.trim().toStdString();
    juce::Component::SafePointer<LibraryBrowser> self(this);
    database->write(
        [id, text, self](library::Connection& c)
        {
            library::PlaylistRepository repo(c);
            auto p = repo.get(id);
            if (!p)
                return;
            p->name = text;
            if (!repo.update(*p))
            {
                const auto error = juce::String::fromUTF8(repo.error().c_str());
                juce::MessageManager::callAsync(
                    [self, error]
                    {
                        if (self != nullptr)
                            self->setStatus("Can't rename it: " + error);
                    });
            }
        });
}

void LibraryBrowser::deletePlaylist(std::int64_t id)
{
    if (database == nullptr)
        return;
    if (shownPlaylist() == id)
        showCollection();
    database->write([id](library::Connection& c) { library::PlaylistRepository(c).remove(id); });
}

void LibraryBrowser::addTracksToPlaylist(std::int64_t playlistId, const std::vector<std::int64_t>& trackIds)
{
    if (database == nullptr || trackIds.empty())
        return;
    const auto info = playlistInfo(playlistId);
    juce::Component::SafePointer<LibraryBrowser> self(this);
    const auto name = info ? juce::String::fromUTF8(info->name.c_str()) : juce::String();
    database->write(
        [playlistId, trackIds, self, name](library::Connection& c)
        {
            library::PlaylistRepository repo(c);
            const bool ok = repo.addTracks(playlistId, trackIds);
            const auto text = ok ? "Added " + juce::String(static_cast<int>(trackIds.size())) +
                                       (trackIds.size() == 1 ? " track to " : " tracks to ") + name
                                 : "Can't add: " + juce::String::fromUTF8(repo.error().c_str());
            juce::MessageManager::callAsync(
                [self, text]
                {
                    if (self != nullptr)
                        self->setStatus(text);
                });
        });
}

bool LibraryBrowser::removeSelectedFromPlaylist()
{
    const auto playlist = shownPlaylist();
    const auto rows = selectedRows();
    if (database == nullptr || playlist <= 0 || rows.empty() || tableModel == nullptr)
        return false;
    // Rows of the table are positions only when the list is in playlist order; otherwise map by track.
    std::vector<int> positions;
    if (canReorder())
        positions = rows;
    else
    {
        std::vector<std::int64_t> ids;
        if (reader != nullptr)
            ids = library::PlaylistRepository(*reader).trackIds(playlist);
        std::vector<bool> used(ids.size(), false);
        for (const auto id : selectedTrackIds())
            for (std::size_t i = 0; i < ids.size(); ++i)
                if (!used[i] && ids[i] == id)
                {
                    used[i] = true;
                    positions.push_back(static_cast<int>(i));
                    break;
                }
    }
    trackTable.deselectAllRows();
    database->write([playlist, positions](library::Connection& c)
                    { library::PlaylistRepository(c).removeTracksAt(playlist, positions); });
    return true;
}

void LibraryBrowser::moveSelectedRows(int insertBefore)
{
    const auto playlist = shownPlaylist();
    if (database == nullptr || !canReorder() || reader == nullptr)
        return;
    auto rows = selectedRows();
    if (rows.empty())
        return;
    auto ids = library::PlaylistRepository(*reader).trackIds(playlist);
    const int n = static_cast<int>(ids.size());
    insertBefore = std::clamp(insertBefore, 0, n);
    std::vector<std::int64_t> moved, rest;
    std::vector<bool> isMoved(ids.size(), false);
    for (const int r : rows)
        if (r >= 0 && r < n)
            isMoved[static_cast<std::size_t>(r)] = true;
    int insertAt = 0;
    for (int i = 0; i < n; ++i)
    {
        if (i == insertBefore)
            insertAt = static_cast<int>(rest.size());
        (isMoved[static_cast<std::size_t>(i)] ? moved : rest).push_back(ids[static_cast<std::size_t>(i)]);
    }
    if (insertBefore >= n)
        insertAt = static_cast<int>(rest.size());
    rest.insert(rest.begin() + insertAt, moved.begin(), moved.end());
    if (rest == ids)
        return;
    // Keep the moved rows selected at their new place (once the table has the new order).
    selectRowsAfterReload.clear();
    for (int i = 0; i < static_cast<int>(moved.size()); ++i)
        selectRowsAfterReload.push_back(insertAt + i);
    database->write([playlist, rest](library::Connection& c) { library::PlaylistRepository(c).setTracks(playlist, rest); });
}

juce::String LibraryBrowser::exportPlaylist(std::int64_t id, const juce::File& file)
{
    if (reader == nullptr)
        return "The library isn't open";
    const auto result =
        library::exportPlaylist(*reader, id, std::filesystem::path(file.getFullPathName().toWideCharPointer()));
    if (!result)
        return juce::String::fromUTF8(result.error().c_str());
    setStatus("Exported " + juce::String(result.value()) + " tracks to " + file.getFileName());
    return {};
}

void LibraryBrowser::askName(const juce::String& title, const juce::String& initial,
                             std::function<void(juce::String)> done)
{
    auto* w = new juce::AlertWindow(title, {}, juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor("name", initial);
    w->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    if (auto* editor = w->getTextEditor("name"))
        editor->selectAll();
    w->enterModalState(true,
                       juce::ModalCallbackFunction::create(
                           [w, done = std::move(done)](int result)
                           {
                               const auto name = w->getTextEditorContents("name").trim();
                               if (result == 1 && name.isNotEmpty())
                                   done(name);
                           }),
                       true);
}

void LibraryBrowser::confirmDelete(std::int64_t id)
{
    const auto info = playlistInfo(id);
    if (!info)
        return;
    const bool folder = info->kind == library::PlaylistKind::Folder;
    const auto name = juce::String::fromUTF8(info->name.c_str());
    juce::Component::SafePointer<LibraryBrowser> self(this);
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, folder ? "Delete folder" : "Delete playlist",
                                       "Delete \"" + name + "\"" + (folder ? " and everything in it?" : "?") +
                                           "\nThe tracks stay in the collection.",
                                       "Delete", "Cancel", this,
                                       juce::ModalCallbackFunction::create(
                                           [self, id](int result)
                                           {
                                               if (result == 1 && self != nullptr) // 1 = Delete, 0 = Cancel
                                                   self->deletePlaylist(id);
                                           }));
}

juce::String LibraryBrowser::exportTracklist(std::int64_t sessionId, const juce::File& file)
{
    if (reader == nullptr)
        return "no library";
    library::HistoryRepository history(*reader);
    const auto result = history.exportTracklist(sessionId, std::filesystem::path(file.getFullPathName().toWideCharPointer()));
    if (!result)
        return juce::String::fromUTF8(result.error().c_str());
    setStatus("Wrote " + juce::String(result.value()) + " tracks to " + file.getFileName());
    return {};
}

void LibraryBrowser::chooseTracklistFile(std::int64_t id)
{
    if (reader == nullptr)
        return;
    const auto p = library::PlaylistRepository(*reader).get(id);
    const auto name = p ? juce::File::createLegalFileName(juce::String::fromUTF8(p->name.c_str())) : juce::String("History");
    exportChooser = std::make_unique<juce::FileChooser>(
        "Export tracklist", juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile(name + ".txt"),
        "*.txt");
    juce::Component::SafePointer<LibraryBrowser> self(this);
    exportChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                               [self, id](const juce::FileChooser& chooser)
                               {
                                   const auto file = chooser.getResult();
                                   if (self == nullptr || file == juce::File())
                                       return;
                                   if (const auto error = self->exportTracklist(id, file); error.isNotEmpty())
                                       self->setStatus("Export failed: " + error);
                               });
}

void LibraryBrowser::chooseExportFile(std::int64_t id)
{
    const auto info = playlistInfo(id);
    if (!info)
        return;
    const auto name = juce::File::createLegalFileName(juce::String::fromUTF8(info->name.c_str()));
    exportChooser = std::make_unique<juce::FileChooser>(
        "Export playlist", juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile(name + ".m3u8"),
        "*.m3u8");
    juce::Component::SafePointer<LibraryBrowser> self(this);
    exportChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                               [self, id](const juce::FileChooser& fc)
                               {
                                   auto file = fc.getResult();
                                   if (self == nullptr || file == juce::File())
                                       return;
                                   if (!file.hasFileExtension(".m3u8;.m3u"))
                                       file = file.withFileExtension(".m3u8");
                                   if (const auto error = self->exportPlaylist(id, file); error.isNotEmpty())
                                       self->setStatus("Export failed: " + error);
                               });
}

juce::PopupMenu LibraryBrowser::menuForTree(std::int64_t id, View section)
{
    juce::PopupMenu m;
    juce::Component::SafePointer<LibraryBrowser> self(this);
    if (tableModel != nullptr && section == View::Missing)
        return menuForMissing();
    if (tableModel != nullptr && section == View::SmartPlaylist)
    {
        if (id == 0)
        {
            m.addItem("New smart playlist...",
                      [self]
                      {
                          if (self != nullptr)
                              self->editSmartPlaylist(0);
                      });
            return m;
        }
        m.addItem("Edit rules...",
                  [self, id]
                  {
                      if (self != nullptr)
                          self->editSmartPlaylist(id);
                  });
        m.addItem("Delete...",
                  [self, id]
                  {
                      if (self != nullptr)
                          self->confirmDelete(id);
                  });
        return m;
    }
    if (tableModel == nullptr || section != View::Playlist)
        return m; // history: P2-23
    const auto info = playlistInfo(id);
    const bool isFolder = id == 0 || (info && info->kind == library::PlaylistKind::Folder);
    const std::optional<std::int64_t> parent = id == 0 ? std::nullopt : std::optional<std::int64_t>(id);
    if (isFolder)
    {
        m.addItem("New playlist",
                  [self, parent]
                  {
                      if (self != nullptr)
                          self->askName("New playlist", self->uniquePlaylistName("New playlist", parent),
                                        [self, parent](juce::String name)
                                        {
                                            if (self != nullptr)
                                                self->createPlaylist(name, parent);
                                        });
                  });
        m.addItem("New folder",
                  [self, parent]
                  {
                      if (self != nullptr)
                          self->askName("New folder", self->uniquePlaylistName("New folder", parent),
                                        [self, parent](juce::String name)
                                        {
                                            if (self != nullptr)
                                                self->createPlaylist(name, parent, true);
                                        });
                  });
        m.addItem("Import M3U / M3U8 playlist...",
                  [self, parent]
                  {
                      if (self != nullptr && self->onImportPlaylist)
                          self->onImportPlaylist(parent);
                  });
    }
    if (id > 0 && info)
    {
        if (isFolder)
            m.addSeparator();
        const auto current = juce::String::fromUTF8(info->name.c_str());
        m.addItem("Rename...",
                  [self, id, current]
                  {
                      if (self != nullptr)
                          self->askName("Rename", current,
                                        [self, id](juce::String name)
                                        {
                                            if (self != nullptr)
                                                self->renamePlaylist(id, name);
                                        });
                  });
        if (!isFolder)
            m.addItem("Export as M3U8...",
                      [self, id]
                      {
                          if (self != nullptr)
                              self->chooseExportFile(id);
                      });
        if (info->kind == library::PlaylistKind::History)
            m.addItem("Export tracklist (text)...",
                      [self, id]
                      {
                          if (self != nullptr)
                              self->chooseTracklistFile(id);
                      });
        m.addItem("Delete...",
                  [self, id]
                  {
                      if (self != nullptr)
                          self->confirmDelete(id);
                  });
    }
    return m;
}

void LibraryBrowser::showTreeMenu(std::int64_t id, View section)
{
    auto m = menuForTree(id, section);
    if (m.getNumItems() > 0)
        m.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

juce::PopupMenu LibraryBrowser::menuForTracks()
{
    juce::PopupMenu m;
    const auto ids = selectedTrackIds();
    if (ids.empty())
        return m;
    juce::Component::SafePointer<LibraryBrowser> self(this);
    m.addItem("Load to deck A", [self]
              {
                  if (self != nullptr)
                      self->loadSelected(0);
              });
    m.addItem("Load to deck B", [self]
              {
                  if (self != nullptr)
                      self->loadSelected(1);
              });
    m.addSeparator();

    // Add to playlist: every playlist, folders as submenus.
    juce::PopupMenu add;
    if (reader != nullptr)
    {
        library::PlaylistRepository repo(*reader);
        std::function<void(juce::PopupMenu&, std::optional<std::int64_t>)> fill =
            [&](juce::PopupMenu& menu, std::optional<std::int64_t> parent)
        {
            for (const auto& p : repo.children(parent))
            {
                const auto name = juce::String::fromUTF8(p.name.c_str());
                if (p.kind == library::PlaylistKind::Folder)
                {
                    juce::PopupMenu sub;
                    fill(sub, p.id);
                    menu.addSubMenu(name, sub, sub.getNumItems() > 0);
                }
                else if (p.kind == library::PlaylistKind::Playlist)
                {
                    const auto playlistId = p.id;
                    menu.addItem(name, p.id != shownPlaylist(),
                                 false, [self, playlistId, ids]
                                 {
                                     if (self != nullptr)
                                         self->addTracksToPlaylist(playlistId, ids);
                                 });
                }
            }
        };
        fill(add, std::nullopt);
    }
    add.addSeparator();
    add.addItem("New playlist with these tracks...",
                [self, ids]
                {
                    if (self == nullptr)
                        return;
                    self->askName("New playlist", self->uniquePlaylistName("New playlist", std::nullopt),
                                  [self, ids](juce::String name)
                                  {
                                      if (self == nullptr || self->database == nullptr)
                                          return;
                                      const auto text = name.toStdString();
                                      self->database->write(
                                          [text, ids](library::Connection& c)
                                          {
                                              library::PlaylistRepository repo(c);
                                              library::Playlist p;
                                              p.name = text;
                                              if (const auto created = repo.create(p))
                                                  repo.setTracks(created.value(), ids);
                                          });
                                  });
                });
    m.addSubMenu("Add to playlist", add);
    m.addSubMenu("Rating", ratingMenu());
    m.addSubMenu("Colour", colourMenu());
    m.addSubMenu("Tags", tagMenu());
    if (shownPlaylist() > 0)
        m.addItem("Remove from playlist (Delete)", [self]
                  {
                      if (self != nullptr)
                          self->removeSelectedFromPlaylist();
                  });
    if (onSeparateStems)
    {
        juce::PopupMenu stems;
        stems.addItem("Fast (about half real time)", [self, ids]
                      {
                          if (self != nullptr && self->onSeparateStems)
                              self->onSeparateStems(ids, false);
                      });
        stems.addItem("Quality (about 3x slower)", [self, ids]
                      {
                          if (self != nullptr && self->onSeparateStems)
                              self->onSeparateStems(ids, true);
                      });
        if (stemsBusy && stemsBusy())
        {
            stems.addSeparator();
            stems.addItem("Cancel stem jobs", [self]
                          {
                              if (self != nullptr && self->onCancelStems)
                                  self->onCancelStems();
                          });
        }
        m.addSubMenu("Separate stems", stems);
        if (stemsFolder && ids.size() == 1)
            if (const auto folder = stemsFolder(ids.front()); folder.isDirectory())
                m.addItem("Show stems in Explorer", [folder] { folder.getChildFile("drums.flac").revealToUser(); });
    }
    m.addSeparator();
    const auto file = selectedTrackFile();
    m.addItem("Show in Explorer", file.existsAsFile(), false, [file] { file.revealToUser(); });
    if (!file.existsAsFile())
        m.addItem("Relocate (search a folder for it)...",
                  [self, ids]
                  {
                      if (self != nullptr && self->onRelocate)
                          self->onRelocate(ids);
                  });
    return m;
}

// --- smart playlists (P2-07) -------------------------------------------------------------------------------------

void LibraryBrowser::createSmartPlaylist(const juce::String& name, const library::SmartRules& rules)
{
    if (database == nullptr)
        return;
    const auto text = name.trim().toStdString();
    const auto json = library::toJson(rules);
    juce::Component::SafePointer<LibraryBrowser> self(this);
    database->write(
        [text, json, self](library::Connection& c)
        {
            library::Playlist p;
            p.name = text;
            p.kind = library::PlaylistKind::Smart;
            p.ruleJson = json;
            const auto created = library::PlaylistRepository(c).create(p);
            const auto id = created ? created.value() : std::int64_t{0};
            juce::MessageManager::callAsync(
                [self, id]
                {
                    if (self != nullptr && id != 0)
                        self->selectAfterRebuild = id;
                });
        });
}

void LibraryBrowser::updateSmartPlaylist(std::int64_t id, const juce::String& name, const library::SmartRules& rules)
{
    if (database == nullptr)
        return;
    const auto text = name.trim().toStdString();
    const auto json = library::toJson(rules);
    database->write(
        [id, text, json](library::Connection& c)
        {
            library::PlaylistRepository repo(c);
            auto p = repo.get(id);
            if (!p || p->kind != library::PlaylistKind::Smart)
                return;
            p->name = text;
            p->ruleJson = json;
            repo.update(*p); // a Playlists change: the tree and the shown tracks refresh
        });
}

void LibraryBrowser::editSmartPlaylist(std::int64_t id)
{
    if (tableModel == nullptr)
        return;
    library::SmartRules rules;
    juce::String name = uniquePlaylistName("New smart playlist", std::nullopt);
    if (id > 0)
    {
        const auto info = playlistInfo(id);
        if (!info)
            return;
        name = juce::String::fromUTF8(info->name.c_str());
        if (!info->ruleJson.empty())
            if (auto parsed = library::parseRules(info->ruleJson))
                rules = std::move(parsed).value();
    }
    else
    {
        // A useful start: a BPM range.
        rules.rules.push_back({library::RuleField::Bpm, library::RuleOp::Between, {}, 120.0, 128.0});
    }
    juce::Component::SafePointer<LibraryBrowser> self(this);
    showSmartPlaylistEditor(
        this, id > 0 ? "Edit smart playlist" : "New smart playlist", name, rules,
        [self](const library::SmartRules& r) { return self != nullptr && self->tableModel != nullptr ? self->tableModel->countMatching(r) : -1; },
        [self, id](const juce::String& n, const library::SmartRules& r)
        {
            if (self == nullptr)
                return;
            if (id > 0)
                self->updateSmartPlaylist(id, n, r);
            else
                self->createSmartPlaylist(n, r);
        });
}

// --- ratings, colours, tags (P2-08) ------------------------------------------------------------------------------

int LibraryBrowser::starAt(int x)
{
    if (x < kStarLeft)
        return 0;
    return std::clamp((x - kStarLeft) / kStarWidth + 1, 1, 5);
}

void LibraryBrowser::rateSelected(int stars)
{
    const auto ids = selectedTrackIds();
    if (database == nullptr || ids.empty())
        return;
    stars = std::clamp(stars, 0, 5);
    database->write(
        [ids, stars](library::Connection& c)
        {
            c.transaction(
                [&]
                {
                    library::TrackRepository repo(c);
                    for (const auto id : ids)
                        if (!repo.setRating(id, stars))
                            return false;
                    return true;
                });
        });
    setStatus(stars == 0 ? juce::String("Rating cleared")
                         : "Rated " + juce::String(stars) + (stars == 1 ? " star" : " stars"));
}

void LibraryBrowser::colourSelected(std::optional<std::uint32_t> rgb)
{
    const auto ids = selectedTrackIds();
    if (database == nullptr || ids.empty())
        return;
    database->write(
        [ids, rgb](library::Connection& c)
        {
            c.transaction(
                [&]
                {
                    library::TrackRepository repo(c);
                    for (const auto id : ids)
                        if (!repo.setColour(id, rgb))
                            return false;
                    return true;
                });
        });
}

void LibraryBrowser::setTagOnSelected(std::int64_t tagId, bool on)
{
    const auto ids = selectedTrackIds();
    if (database == nullptr || ids.empty())
        return;
    database->write(
        [ids, tagId, on](library::Connection& c)
        {
            c.transaction(
                [&]
                {
                    library::TagRepository repo(c);
                    for (const auto id : ids)
                        if (!(on ? repo.assign(id, tagId) : repo.unassign(id, tagId)))
                            return false;
                    return true;
                });
        });
}

namespace
{
void reportFailure(juce::Component::SafePointer<LibraryBrowser> self, const std::string& what, const std::string& why)
{
    const auto text = juce::String::fromUTF8((what + ": " + why).c_str());
    juce::MessageManager::callAsync(
        [self, text]
        {
            if (self != nullptr)
                self->setStatus(text);
        });
}
} // namespace

void LibraryBrowser::createTag(const juce::String& name, std::optional<std::int64_t> categoryId, bool assign)
{
    if (database == nullptr)
        return;
    const auto text = name.trim().toStdString();
    const auto ids = assign ? selectedTrackIds() : std::vector<std::int64_t>{};
    juce::Component::SafePointer<LibraryBrowser> self(this);
    database->write(
        [text, categoryId, ids, self](library::Connection& c)
        {
            library::TagRepository repo(c);
            const auto created = repo.createTag(text, categoryId);
            if (!created)
            {
                reportFailure(self, "Can't make the tag", created.error());
                return;
            }
            for (const auto id : ids)
                repo.assign(id, created.value());
        });
}

void LibraryBrowser::createTagCategory(const juce::String& name)
{
    if (database == nullptr)
        return;
    const auto text = name.trim().toStdString();
    juce::Component::SafePointer<LibraryBrowser> self(this);
    database->write(
        [text, self](library::Connection& c)
        {
            const auto created = library::TagRepository(c).createCategory(text);
            if (!created)
                reportFailure(self, "Can't make the category", created.error());
        });
}

void LibraryBrowser::renameTag(std::int64_t tagId, const juce::String& name)
{
    if (database == nullptr)
        return;
    const auto text = name.trim().toStdString();
    juce::Component::SafePointer<LibraryBrowser> self(this);
    database->write(
        [tagId, text, self](library::Connection& c)
        {
            library::TagRepository repo(c);
            auto t = repo.tag(tagId);
            if (!t)
                return;
            t->name = text;
            if (!repo.updateTag(*t))
                reportFailure(self, "Can't rename the tag", repo.error());
        });
}

void LibraryBrowser::deleteTag(std::int64_t tagId)
{
    if (database != nullptr)
        database->write([tagId](library::Connection& c) { library::TagRepository(c).removeTag(tagId); });
}

void LibraryBrowser::renameTagCategory(std::int64_t categoryId, const juce::String& name)
{
    if (database == nullptr)
        return;
    const auto text = name.trim().toStdString();
    juce::Component::SafePointer<LibraryBrowser> self(this);
    database->write(
        [categoryId, text, self](library::Connection& c)
        {
            library::TagRepository repo(c);
            auto cat = repo.category(categoryId);
            if (!cat)
                return;
            cat->name = text;
            if (!repo.updateCategory(*cat))
                reportFailure(self, "Can't rename the category", repo.error());
        });
}

void LibraryBrowser::deleteTagCategory(std::int64_t categoryId)
{
    if (database != nullptr)
        database->write([categoryId](library::Connection& c) { library::TagRepository(c).removeCategory(categoryId); });
}

juce::PopupMenu LibraryBrowser::ratingMenu()
{
    juce::PopupMenu m;
    const auto* first = tableModel != nullptr ? tableModel->row(trackTable.getSelectedRow()) : nullptr;
    juce::Component::SafePointer<LibraryBrowser> self(this);
    for (int r = 5; r >= 0; --r)
        m.addItem((r == 0 ? juce::String("No rating") : stars(r)) + "   (Ctrl+Shift+" + juce::String(r) + ")", true,
                  first != nullptr && first->rating == r,
                  [self, r]
                  {
                      if (self != nullptr)
                          self->rateSelected(r);
                  });
    return m;
}

juce::PopupMenu LibraryBrowser::colourMenu()
{
    juce::PopupMenu m;
    const auto* first = tableModel != nullptr ? tableModel->row(trackTable.getSelectedRow()) : nullptr;
    juce::Component::SafePointer<LibraryBrowser> self(this);
    for (const auto& c : kTrackColours)
    {
        juce::PopupMenu::Item item(juce::String::fromUTF8("\xE2\x97\x8F  ") + c.name); // ● name
        item.colour = toColour(c.rgb);
        item.isTicked = first != nullptr && first->colour == c.rgb;
        const auto rgb = c.rgb;
        item.action = [self, rgb]
        {
            if (self != nullptr)
                self->colourSelected(rgb);
        };
        m.addItem(std::move(item));
    }
    m.addSeparator();
    m.addItem("No colour", true, first != nullptr && !first->colour,
              [self]
              {
                  if (self != nullptr)
                      self->colourSelected(std::nullopt);
              });
    return m;
}

juce::PopupMenu LibraryBrowser::tagMenu()
{
    juce::PopupMenu m;
    if (reader == nullptr)
        return m;
    juce::Component::SafePointer<LibraryBrowser> self(this);
    library::TagRepository repo(*reader);

    // How many of the selected tracks carry each tag: ticked when all do.
    const auto ids = selectedTrackIds();
    std::map<std::int64_t, int> carriers;
    for (const auto id : ids)
        for (const auto& t : repo.tagsForTrack(id))
            ++carriers[t.id];
    const auto addTags = [&](juce::PopupMenu& menu, std::optional<std::int64_t> category)
    {
        for (const auto& t : repo.tagsInCategory(category))
        {
            const bool all = !ids.empty() && carriers[t.id] == static_cast<int>(ids.size());
            const auto tagId = t.id;
            menu.addItem(juce::String::fromUTF8(t.name.c_str()), !ids.empty(), all,
                         [self, tagId, all]
                         {
                             if (self != nullptr)
                                 self->setTagOnSelected(tagId, !all); // toggle
                         });
        }
    };
    const auto askNewTag = [self](std::optional<std::int64_t> category)
    {
        return [self, category]
        {
            if (self != nullptr)
                self->askName("New tag", {},
                              [self, category](juce::String name)
                              {
                                  if (self != nullptr)
                                      self->createTag(name, category, true);
                              });
        };
    };

    for (const auto& cat : repo.categories())
    {
        juce::PopupMenu sub;
        addTags(sub, cat.id);
        sub.addSeparator();
        sub.addItem("New tag in " + juce::String::fromUTF8(cat.name.c_str()) + "...", askNewTag(cat.id));
        m.addSubMenu(juce::String::fromUTF8(cat.name.c_str()), sub);
    }
    addTags(m, std::nullopt);
    m.addSeparator();
    m.addItem("New tag...", askNewTag(std::nullopt));
    m.addItem("New category...",
              [self]
              {
                  if (self != nullptr)
                      self->askName("New tag category", {},
                                    [self](juce::String name)
                                    {
                                        if (self != nullptr)
                                            self->createTagCategory(name);
                                    });
              });

    // Rename / delete, one place for all of them.
    juce::PopupMenu manage;
    for (const auto& cat : repo.categories())
    {
        juce::PopupMenu one;
        const auto catId = cat.id;
        const auto catName = juce::String::fromUTF8(cat.name.c_str());
        one.addItem("Rename category...",
                    [self, catId, catName]
                    {
                        if (self != nullptr)
                            self->askName("Rename category", catName,
                                          [self, catId](juce::String name)
                                          {
                                              if (self != nullptr)
                                                  self->renameTagCategory(catId, name);
                                          });
                    });
        one.addItem("Delete category and its tags",
                    [self, catId]
                    {
                        if (self != nullptr)
                            self->deleteTagCategory(catId);
                    });
        manage.addSubMenu(catName, one);
    }
    for (const auto& t : repo.tags())
    {
        juce::PopupMenu one;
        const auto tagId = t.id;
        const auto tagName = juce::String::fromUTF8(t.name.c_str());
        one.addItem("Rename tag...",
                    [self, tagId, tagName]
                    {
                        if (self != nullptr)
                            self->askName("Rename tag", tagName,
                                          [self, tagId](juce::String name)
                                          {
                                              if (self != nullptr)
                                                  self->renameTag(tagId, name);
                                          });
                    });
        one.addItem("Delete tag",
                    [self, tagId]
                    {
                        if (self != nullptr)
                            self->deleteTag(tagId);
                    });
        manage.addSubMenu("Tag: " + tagName, one);
    }
    if (manage.getNumItems() > 0)
        m.addSubMenu("Rename / delete", manage);
    return m;
}

// --- missing files (P2-10) ----------------------------------------------------------------------------------------

void LibraryBrowser::showMissing()
{
    for (int i = 0; i < treeView.getNumRowsInTree(); ++i)
        if (auto* item = dynamic_cast<Item*>(treeView.getItemOnRow(i)); item != nullptr && item->itemView == View::Missing)
        {
            item->setSelected(true, true, juce::dontSendNotification);
            break;
        }
    setView(View::Missing, 0);
}

juce::PopupMenu LibraryBrowser::menuForMissing()
{
    juce::PopupMenu m;
    juce::Component::SafePointer<LibraryBrowser> self(this);
    m.addItem("Check for missing files now",
              [self]
              {
                  if (self != nullptr && self->onCheckMissing)
                      self->onCheckMissing();
              });
    m.addItem("Relocate missing files (search a folder)...",
              [self]
              {
                  if (self != nullptr && self->onRelocate)
                      self->onRelocate({});
              });
    return m;
}

} // namespace devildj::ui
