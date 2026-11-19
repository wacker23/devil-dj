// P2-06: playlists in the browser - create / rename / delete, folders, add (drag or menu), reorder, remove.

#include "engine/AudioEngine.h"
#include "library/Connection.h"
#include "library/Database.h"
#include "library/PlaylistRepository.h"
#include "library/TrackRepository.h"
#include "ui/DeckView.h"
#include "ui/LibraryBrowser.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <vector>

using namespace devildj;
namespace fs = std::filesystem;
using View = ui::LibraryBrowser::View;

namespace
{
struct Rig
{
    fs::path dir;
    std::unique_ptr<library::Database> db;
    std::unique_ptr<library::Connection> reader;
    std::vector<std::int64_t> tracks;
    std::unique_ptr<ui::LibraryBrowser> browser;

    Rig()
    {
        dir = fs::temp_directory_path() /
              ("devildj-playlists-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        db = std::move(library::Database::open(dir / "library.db")).value();
        reader = std::move(db->openReader()).value();
        db->writeAndWait(
            [&](library::Connection& c)
            {
                for (const char* title : {"Alpha", "Bravo", "Charlie", "Delta"})
                {
                    library::Track t;
                    t.path = std::string("C:\\Music\\") + title + ".mp3";
                    t.contentHash = t.path;
                    t.title = title;
                    tracks.push_back(library::TrackRepository(c).add(t).value());
                }
            });
        browser = std::make_unique<ui::LibraryBrowser>(db.get(), juce::File());
        browser->setBounds(0, 0, 1200, 400);
    }
    ~Rig()
    {
        browser.reset();
        reader.reset();
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // What the app's message loop does after a write: the write runs, the browser refreshes.
    void settle()
    {
        db->flush();
        browser->handlePendingLibraryChanges();
    }

    std::vector<library::Playlist> all() { return library::PlaylistRepository(*reader).all(); }
    std::optional<library::Playlist> named(const std::string& name)
    {
        for (const auto& p : all())
            if (p.name == name)
                return p;
        return std::nullopt;
    }
    std::vector<std::int64_t> contents(std::int64_t id) { return library::PlaylistRepository(*reader).trackIds(id); }
    void selectRows(std::initializer_list<int> rows)
    {
        browser->table().deselectAllRows();
        for (const int r : rows)
            browser->table().selectRow(r, false, false);
    }
};

juce::StringArray itemNames(const juce::PopupMenu& m)
{
    juce::StringArray names;
    for (juce::PopupMenu::MenuItemIterator it(m); it.next();)
        if (!it.getItem().isSeparator)
            names.add(it.getItem().text);
    return names;
}
} // namespace

TEST_CASE("Playlists: create, folders, rename, delete", "[ui][playlists]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    Rig rig;
    auto& b = *rig.browser;

    b.createPlaylist("Gigs", std::nullopt, true);
    b.createPlaylist("Warm-up", std::nullopt);
    rig.settle();
    const auto gigs = rig.named("Gigs");
    REQUIRE(gigs.has_value());
    CHECK(gigs->kind == library::PlaylistKind::Folder);
    b.createPlaylist("Friday", gigs->id);
    rig.settle();
    CHECK(b.treeRowNames().joinIntoString("|") ==
          "Collection|Missing files|Playlists|Gigs|Friday|Warm-up|Smart playlists|History|Folders");

    // Names: a free default name next to taken ones.
    CHECK(b.uniquePlaylistName("New playlist", std::nullopt) == "New playlist");
    b.createPlaylist("New playlist", std::nullopt);
    rig.settle();
    CHECK(b.uniquePlaylistName("New playlist", std::nullopt) == "New playlist 2");
    CHECK(b.uniquePlaylistName("new PLAYLIST", std::nullopt) == "new PLAYLIST 2");
    CHECK(b.uniquePlaylistName("New playlist", gigs->id) == "New playlist"); // other folder

    const auto warm = *rig.named("Warm-up");
    b.renamePlaylist(warm.id, "  Opening set ");
    rig.settle();
    CHECK(b.treeRowNames().contains("Opening set"));
    CHECK_FALSE(rig.named("Warm-up").has_value());

    // Deleting the shown playlist goes back to the collection; deleting a folder takes its playlists.
    b.showPlaylist(warm.id);
    CHECK(b.view() == View::Playlist);
    b.deletePlaylist(warm.id);
    rig.settle();
    CHECK(b.view() == View::Collection);
    CHECK_FALSE(b.treeRowNames().contains("Opening set"));
    b.deletePlaylist(gigs->id);
    rig.settle();
    CHECK_FALSE(rig.named("Friday").has_value());
    CHECK(rig.all().size() == 1); // "New playlist"
}

TEST_CASE("Playlists: add tracks, reorder, remove", "[ui][playlists]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    Rig rig;
    auto& b = *rig.browser;
    const auto& t = rig.tracks;
    b.createPlaylist("Set", std::nullopt);
    rig.settle();
    const auto set = rig.named("Set")->id;

    // From the collection: several selected tracks at once, then one more.
    b.showCollection();
    rig.selectRows({0, 2});
    CHECK(b.selectedTrackIds() == std::vector<std::int64_t>{t[0], t[2]});
    b.addTracksToPlaylist(set, b.selectedTrackIds());
    b.addTracksToPlaylist(set, {t[3], t[0]}); // a track may be in twice
    rig.settle();
    CHECK(rig.contents(set) == std::vector<std::int64_t>{t[0], t[2], t[3], t[0]});

    // Reorder by dragging rows (insert before row N; N = rows means the end).
    b.showPlaylist(set);
    REQUIRE(b.canReorder());
    rig.selectRows({2}); // Delta
    b.moveSelectedRows(0);
    rig.settle();
    CHECK(rig.contents(set) == std::vector<std::int64_t>{t[3], t[0], t[2], t[0]});
    CHECK(b.selectedRows() == std::vector<int>{0}); // the moved row stays selected
    rig.selectRows({0, 1});
    b.moveSelectedRows(4);
    rig.settle();
    CHECK(rig.contents(set) == std::vector<std::int64_t>{t[2], t[0], t[3], t[0]});
    rig.selectRows({3});
    b.moveSelectedRows(1);
    rig.settle();
    CHECK(rig.contents(set) == std::vector<std::int64_t>{t[2], t[0], t[0], t[3]});

    // No reordering while the list is sorted by another column or filtered (rows aren't positions then).
    b.table().getHeader().setSortColumnId(static_cast<int>(ui::TrackColumn::Title), true);
    b.syncWithHeader();
    CHECK_FALSE(b.canReorder());
    rig.selectRows({0});
    b.moveSelectedRows(3);
    rig.settle();
    CHECK(rig.contents(set) == std::vector<std::int64_t>{t[2], t[0], t[0], t[3]});

    // Remove (Delete) still works sorted: by track, one occurrence each.
    rig.selectRows({0}); // sorted by title: Alpha (one of the two)
    REQUIRE(b.selectedTrackIds() == std::vector<std::int64_t>{t[0]});
    CHECK(b.removeSelectedFromPlaylist());
    rig.settle();
    CHECK(rig.contents(set) == std::vector<std::int64_t>{t[2], t[0], t[3]});

    b.table().getHeader().setSortColumnId(static_cast<int>(ui::TrackColumn::Number), true);
    b.syncWithHeader();
    rig.selectRows({0, 2});
    CHECK(b.removeSelectedFromPlaylist());
    rig.settle();
    CHECK(rig.contents(set) == std::vector<std::int64_t>{t[0]});
    CHECK(rig.reader->queryInt("SELECT count(*) FROM tracks") == 4); // the tracks stay in the collection

    // Not in a playlist: nothing to remove.
    b.showCollection();
    rig.selectRows({1});
    CHECK_FALSE(b.removeSelectedFromPlaylist());
}

TEST_CASE("Playlists: drag tracks onto the tree, and rows onto decks", "[ui][playlists]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    Rig rig;
    auto& b = *rig.browser;
    b.createPlaylist("Target", std::nullopt);
    rig.settle();
    const auto target = rig.named("Target")->id;
    b.showCollection();
    rig.selectRows({1, 3});

    // The drag description: the first file (for decks) and every track id (for playlists).
    juce::SparseSet<int> rows;
    rows.addRange({1, 2});
    rows.addRange({3, 4});
    const auto description = b.table().getTableListBoxModel()->getDragSourceDescription(rows);
    const auto* o = description.getDynamicObject();
    REQUIRE(o != nullptr);
    CHECK(o->getProperty("path").toString() == "C:\\Music\\Bravo.mp3");
    REQUIRE(o->getProperty("trackIds").getArray() != nullptr);
    CHECK(o->getProperty("trackIds").getArray()->size() == 2);

    // Dropped on the playlist in the tree.
    juce::TreeViewItem* targetItem = nullptr;
    for (int i = 0; i < b.tree().getNumRowsInTree(); ++i)
        if (b.treeRowNames()[i] == "Target")
            targetItem = b.tree().getItemOnRow(i);
    REQUIRE(targetItem != nullptr);
    juce::Component source;
    const juce::DragAndDropTarget::SourceDetails details(description, &source, {});
    CHECK(targetItem->isInterestedInDragSource(details));
    targetItem->itemDropped(details, 0);
    rig.settle();
    CHECK(rig.contents(target) == std::vector<std::int64_t>{rig.tracks[1], rig.tracks[3]});

    // Section headings and folders don't take tracks.
    juce::TreeViewItem* heading = b.tree().getItemOnRow(2); // "Playlists"
    CHECK_FALSE(heading->isInterestedInDragSource(details));

    // A deck loads the first dragged track.
    engine::AudioEngine engine;
    ui::DeckView deck(engine, 0, [](engine::BufferId) { return nullptr; });
    juce::File dropped;
    deck.onTrackDropped = [&](const juce::File& f) { dropped = f; };
    CHECK(deck.isInterestedInDragSource(details));
    deck.itemDropped(details);
    CHECK(dropped.getFileName() == "Bravo.mp3");
}

TEST_CASE("Playlists: the right-click menus", "[ui][playlists]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    Rig rig;
    auto& b = *rig.browser;
    b.createPlaylist("Folder", std::nullopt, true);
    b.createPlaylist("List", std::nullopt);
    rig.settle();
    const auto folder = rig.named("Folder")->id;
    const auto list = rig.named("List")->id;

    CHECK(itemNames(b.menuForTree(0, View::Playlist)) ==
          juce::StringArray{"New playlist", "New folder", "Import M3U / M3U8 playlist..."});
    CHECK(itemNames(b.menuForTree(folder, View::Playlist)) ==
          juce::StringArray{"New playlist", "New folder", "Import M3U / M3U8 playlist...", "Rename...", "Delete..."});
    CHECK(itemNames(b.menuForTree(list, View::Playlist)) ==
          juce::StringArray{"Rename...", "Export as M3U8...", "Delete..."});
    CHECK(b.menuForTree(0, View::Collection).getNumItems() == 0);

    b.showCollection();
    rig.selectRows({0});
    const auto tracks = itemNames(b.menuForTracks());
    CHECK(tracks.contains("Add to playlist"));
    CHECK(tracks.contains("Load to deck A"));
    CHECK_FALSE(tracks.contains("Remove from playlist (Delete)"));
    b.showPlaylist(list);
    b.addTracksToPlaylist(list, {rig.tracks[0]});
    rig.settle();
    rig.selectRows({0});
    CHECK(itemNames(b.menuForTracks()).contains("Remove from playlist (Delete)"));

    // Export from the browser.
    const auto file = juce::File(juce::String((rig.dir / "List.m3u8").wstring().c_str()));
    CHECK(b.exportPlaylist(list, file).isEmpty());
    CHECK(file.loadFileAsString().contains("C:\\Music\\Alpha.mp3"));
    CHECK(b.status().contains("Exported 1 tracks"));
}
