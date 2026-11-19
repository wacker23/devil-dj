#include "LibraryBrowser.h"

#include "engine/TrackDecoder.h"
#include "library/Connection.h"
#include "library/Database.h"
#include "library/PlaylistRepository.h"
#include "library/TagRepository.h"

#include <algorithm>

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

    void itemClicked(const juce::MouseEvent&) override { browser.treeFocused = true; }
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
    trackTable.setMultipleSelectionEnabled(false);
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
    for (auto* l : {&statusLabel, &countLabel})
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
            keyBox.addItem(TrackTableModel::keyName(pitch + 12 * minor), 2 + pitch + 12 * minor);
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
                                                           &loadB, &statusLabel, &countLabel, &search, &filterButton})
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
                    tracks |= c.table == library::Table::Tracks || c.table == library::Table::PlaylistTracks;
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

    // Re-select what was shown.
    for (int i = 0; i < treeView.getNumRowsInTree(); ++i)
        if (auto* item = dynamic_cast<Item*>(treeView.getItemOnRow(i));
            item != nullptr && !item->isFolder && item->itemView == currentView &&
            (currentView == View::Collection || currentView == View::Folders || item->playlistId == currentPlaylist))
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
        // Smart playlists (P2-07) and history (P2-23) have no stored tracks yet: an empty list.
        TrackSource s;
        if (v == View::Playlist || v == View::SmartPlaylist || v == View::History)
            s = {TrackSource::Kind::Playlist, playlistId};
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
        case TrackColumn::Rating: text = stars(r->rating); break;
        case TrackColumn::Genre: text = r->genre; break;
        case TrackColumn::DateAdded: text = dateText(r->dateAdded); break;
        case TrackColumn::Comment: text = r->comment; break;
        case TrackColumn::Colour: break;
    }
    g.setColour(selected ? juce::Colours::white : (col == TrackColumn::Number ? kDim : kText));
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

void LibraryBrowser::cellClicked(int, int, const juce::MouseEvent&)
{
    treeFocused = false;
    treeView.repaint();
}

void LibraryBrowser::cellDoubleClicked(int row, int, const juce::MouseEvent&)
{
    trackTable.selectRow(row);
    loadSelected(0);
}

juce::var LibraryBrowser::getDragSourceDescription(const juce::SparseSet<int>& rows)
{
    // Dragged onto a deck: the deck's TrackDropTarget reads the file path.
    if (rows.isEmpty() || tableModel == nullptr)
        return {};
    if (const auto* r = tableModel->row(rows[0]))
        return r->path;
    return {};
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
        if (selectedTrackId() != 0)
            setStatus("File not found: " + file.getFullPathName());
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
        const auto keep = selectedTrackId();
        tableModel->reload();
        trackTable.updateContent();
        if (keep != 0)
            selectTrack(keep);
        trackTable.repaint();
        updateCount();
    }
}

void LibraryBrowser::setBig(bool big)
{
    sizeButton.setButtonText(big ? "SMALLER" : "BIGGER");
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
            colourBox.addItem(colourName(rgb) + juce::String::formatted(" (#%06X)", static_cast<unsigned>(rgb)), id);
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
        if (const juce::File f(path); f.isDirectory() || engine::isSupportedTrackFile(f))
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
        if (const juce::File f(path); f.isDirectory() || engine::isSupportedTrackFile(f))
            toImport.add(f);
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
                                  : currentView == View::SmartPlaylist ? "Smart playlist rules come in a later version."
                                  : currentView == View::History       ? "Played tracks will appear here."
                                                                       : "Empty playlist.";
        g.drawFittedText(text, where.reduced(20), juce::Justification::centredTop, 2);
    }
}

} // namespace devildj::ui
