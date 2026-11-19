// P2-04: library browser - tree, virtualized track table, loading, change refresh, column layout, 50k rows.
// Done when: scrolling 50k rows at 60 fps; column layout persists.

#include "Settings.h"
#include "SettingsKeys.h"
#include "TestData.h"
#include "library/Connection.h"
#include "library/Database.h"
#include "library/PlaylistRepository.h"
#include "library/TrackRepository.h"
#include "ui/DeckView.h"
#include "ui/LibraryBrowser.h"
#include "ui/TrackTableModel.h"

#include "engine/AudioEngine.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <numeric>
#include <vector>

using namespace devildj;
using ui::TrackColumn;
namespace fs = std::filesystem;

namespace
{
struct TestLibrary
{
    fs::path dir;
    std::unique_ptr<library::Database> db;
    std::unique_ptr<library::Connection> reader;

    explicit TestLibrary(const char* name)
    {
        dir = fs::temp_directory_path() /
              (std::string("devildj-browser-") + name + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        auto opened = library::Database::open(dir / "library.db");
        REQUIRE(opened.isOk());
        db = std::move(opened).value();
        auto r = db->openReader();
        REQUIRE(r.isOk());
        reader = std::move(r).value();
    }
    ~TestLibrary()
    {
        reader.reset();
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    std::int64_t add(const std::string& title, std::optional<double> bpm, const std::string& path = {})
    {
        std::int64_t id = 0;
        db->writeAndWait(
            [&](library::Connection& c)
            {
                library::Track t;
                t.path = path.empty() ? "C:\\Music\\" + title + ".mp3" : path;
                t.contentHash = "h-" + t.path;
                t.title = title;
                t.artist = "Artist of " + title;
                t.bpm = bpm;
                t.musicalKey = 21;
                t.durationMs = 307000;
                t.rating = 3;
                auto r = library::TrackRepository(c).add(t);
                CHECK(r.isOk());
                id = r ? r.value() : 0;
            });
        return id;
    }

    std::int64_t playlist(const std::string& name, std::vector<std::int64_t> tracks,
                          std::optional<std::int64_t> parent = std::nullopt,
                          library::PlaylistKind kind = library::PlaylistKind::Playlist)
    {
        std::int64_t id = 0;
        db->writeAndWait(
            [&](library::Connection& c)
            {
                library::PlaylistRepository repo(c);
                id = repo.create({0, parent, name, kind}).value();
                if (!tracks.empty())
                    CHECK(repo.setTracks(id, tracks));
            });
        return id;
    }

    // 50,000 tracks in one statement (generated, like a big real library).
    void fill(int count)
    {
        db->writeAndWait(
            [&](library::Connection& c)
            {
                CHECK(c.exec("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < " +
                             std::to_string(count) +
                             ") INSERT INTO tracks (path, content_hash, title, artist, genre, bpm, musical_key, "
                             "duration_ms, rating, color, date_added, comment) SELECT 'C:\\Music\\' || i || '.mp3', "
                             "'h' || i, 'Track ' || printf('%05d', (i * 7919) % 50000), 'Artist ' || (i % 500), "
                             "'Genre ' || (i % 20), 90 + (i % 80), i % 24, 180000 + i, i % 6, "
                             "CASE WHEN i % 7 = 0 THEN 14692400 ELSE NULL END, 1763400000 + i, 'comment ' || i "
                             "FROM n"));
            });
    }
};

std::vector<juce::String> titles(ui::TrackTableModel& m)
{
    std::vector<juce::String> out;
    for (int i = 0; i < m.size(); ++i)
        out.push_back(m.row(i) != nullptr ? m.row(i)->title : juce::String("?"));
    return out;
}

std::vector<int> visibleColumnIds(const juce::TableHeaderComponent& h)
{
    std::vector<int> ids;
    for (int i = 0; i < h.getNumColumns(true); ++i)
        ids.push_back(h.getColumnIdOfIndex(i, true));
    return ids;
}
} // namespace

// --- the table model ------------------------------------------------------------------------------------------------

TEST_CASE("Track table: key names and times", "[ui][library]")
{
    CHECK(ui::TrackTableModel::keyName(0) == "C");
    CHECK(ui::TrackTableModel::keyName(21) == "Am");
    CHECK(ui::TrackTableModel::keyName(18) == "F#m");
    CHECK(ui::TrackTableModel::keyName(3) == "Eb");
    CHECK(ui::TrackTableModel::keyName(24).isEmpty());
    CHECK(ui::TrackTableModel::timeText(307.4) == "5:07");
    CHECK(ui::TrackTableModel::timeText(59.6) == "1:00");
    CHECK(ui::TrackTableModel::timeText(0.0).isEmpty());
}

TEST_CASE("Track table: collection and playlists, sorted, empty values last", "[ui][library]")
{
    TestLibrary lib("model");
    const auto c = lib.add("Charlie", 126.0);
    const auto a = lib.add("alpha", 122.0);
    const auto b = lib.add("Bravo", std::nullopt); // no BPM
    const auto p = lib.playlist("Set", {b, c, b});

    ui::TrackTableModel model(*lib.reader);
    CHECK(model.size() == 3);
    CHECK(titles(model) == std::vector<juce::String>{"Charlie", "alpha", "Bravo"}); // import order (#)

    model.setSort(TrackColumn::Title, true);
    CHECK(titles(model) == std::vector<juce::String>{"alpha", "Bravo", "Charlie"}); // ignoring case
    model.setSort(TrackColumn::Title, false);
    CHECK(titles(model) == std::vector<juce::String>{"Charlie", "Bravo", "alpha"});
    model.setSort(TrackColumn::Bpm, true);
    CHECK(titles(model) == std::vector<juce::String>{"alpha", "Charlie", "Bravo"});
    model.setSort(TrackColumn::Bpm, false);
    CHECK(titles(model) == std::vector<juce::String>{"Charlie", "alpha", "Bravo"}); // no BPM stays last

    const auto* row = model.row(model.rowOfTrack(a));
    REQUIRE(row != nullptr);
    CHECK(row->artist == "Artist of alpha");
    CHECK(row->key == "Am");
    CHECK(row->bpm == 122.0);
    CHECK(row->rating == 3);
    CHECK(row->seconds == 307.0);
    CHECK(row->path == "C:\\Music\\alpha.mp3");

    // A playlist: its own order, the same track twice.
    model.setSort(TrackColumn::Number, true);
    model.setSource({ui::TrackSource::Kind::Playlist, p});
    CHECK(titles(model) == std::vector<juce::String>{"Bravo", "Charlie", "Bravo"});
    model.setSort(TrackColumn::Title, true);
    CHECK(titles(model) == std::vector<juce::String>{"Bravo", "Bravo", "Charlie"});

    CHECK(model.row(-1) == nullptr);
    CHECK(model.row(3) == nullptr);
}

TEST_CASE("Track table: rows are read a page at a time, on demand", "[ui][library]")
{
    TestLibrary lib("pages");
    lib.fill(50000);
    ui::TrackTableModel model(*lib.reader);
    REQUIRE(model.size() == 50000);
    CHECK(model.pageLoads() == 0); // only the ids so far

    REQUIRE(model.row(0) != nullptr);
    REQUIRE(model.row(199) != nullptr);
    CHECK(model.pageLoads() == 1);
    REQUIRE(model.row(25000) != nullptr);
    CHECK(model.pageLoads() == 2);
    REQUIRE(model.row(0) != nullptr);
    CHECK(model.pageLoads() == 2); // still cached

    // Only the most recent pages are kept.
    for (int p = 0; p < 40; ++p)
        model.row(p * ui::TrackTableModel::kPageSize);
    const int before = model.pageLoads();
    model.row(0); // page 0 was pushed out by the 32 most recent ones
    CHECK(model.pageLoads() == before + 1);

    // Sorting 50k rows only re-reads the id list.
    const auto start = std::chrono::steady_clock::now();
    model.setSort(TrackColumn::Title, true);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    WARN("Sorting 50,000 tracks by title: " << ms << " ms");
    CHECK(ms < 500.0);
    CHECK(model.row(0)->title == "Track 00000");
    CHECK(model.row(49999)->title == "Track 49999");
}

// --- the browser --------------------------------------------------------------------------------------------------

TEST_CASE("Library browser: tree, views, keyboard and loading", "[ui][library]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TestLibrary lib("browser");
    const auto source = test::generatedFile("formats/ref.wav");
    if (!source.existsAsFile())
        SKIP("Generated test signals missing");
    const auto realFile = lib.dir / "real.wav";
    fs::copy_file(fs::path(source.getFullPathName().toWideCharPointer()), realFile);
    const auto real = juce::String(realFile.wstring().c_str());
    const auto t1 = lib.add("Opener", 120.0, real.toStdString());
    const auto t2 = lib.add("Closer", 128.0);
    const auto folder = lib.playlist("Gigs", {}, std::nullopt, library::PlaylistKind::Folder);
    const auto set = lib.playlist("Friday", {t2, t1}, folder);
    lib.playlist("Fast ones", {}, std::nullopt, library::PlaylistKind::Smart);

    ui::LibraryBrowser browser(lib.db.get(), juce::File());
    browser.setBounds(0, 0, 1200, 400);
    std::vector<std::pair<int, juce::File>> loads;
    browser.onLoad = [&](int deck, const juce::File& f) { loads.emplace_back(deck, f); };

    // Tree: the five sections, playlists inside their folder, smart playlists in their own section.
    CHECK(browser.treeRowNames().joinIntoString("|") ==
          "Collection|Playlists|Gigs|Friday|Smart playlists|Fast ones|History|Folders");

    CHECK(browser.view() == ui::LibraryBrowser::View::Collection);
    CHECK(browser.model()->size() == 2);
    CHECK(browser.table().getNumRows() == 2);
    CHECK(browser.table().getViewport()->getViewPositionY() == 0); // row 1 at the top (it once started a row down)

    // Keyboard: Down selects the first row, Tab / Enter load it.
    browser.moveSelection(1);
    CHECK(browser.selectedTrackId() == t1);
    CHECK(browser.loadSelected(1));
    REQUIRE(loads.size() == 1);
    CHECK(loads[0].first == 1);
    CHECK(loads[0].second.getFileName() == "real.wav");
    browser.moveSelection(5); // clamps at the end
    CHECK(browser.selectedTrackId() == t2);
    CHECK(browser.loadSelected(0)); // the file isn't there: reported, not loaded
    CHECK(loads.size() == 1);
    CHECK(browser.status().contains("File not found"));

    // Left: into the tree; Down moves through the selectable items (folders are skipped); Right: back.
    browser.collapse();
    CHECK(browser.treeHasFocus());
    browser.moveSelection(1); // Collection -> Friday (Playlists and Gigs are folders)
    CHECK(browser.view() == ui::LibraryBrowser::View::Playlist);
    CHECK(browser.model()->size() == 2);
    CHECK(browser.model()->trackIdAt(0) == t2); // playlist order
    browser.expand();
    CHECK_FALSE(browser.treeHasFocus());
    browser.moveSelection(1);
    CHECK(browser.selectedTrackId() == t2);

    browser.collapse();
    browser.moveSelection(1); // -> Fast ones (smart: no tracks yet)
    CHECK(browser.view() == ui::LibraryBrowser::View::SmartPlaylist);
    CHECK(browser.table().getNumRows() == 0);
    browser.moveSelection(1); // -> Folders (History has no entries yet)
    CHECK(browser.view() == ui::LibraryBrowser::View::Folders);
    CHECK(browser.folders().isVisible());
    CHECK_FALSE(browser.table().isVisible());
    browser.moveSelection(-10);
    CHECK(browser.view() == ui::LibraryBrowser::View::Collection);

    browser.showPlaylist(set);
    CHECK(browser.view() == ui::LibraryBrowser::View::Playlist);
    CHECK(browser.model()->size() == 2);

    // Painting works in every view (and with an empty one).
    for (auto v : {0, 1, 2})
    {
        if (v == 0)
            browser.showCollection();
        else if (v == 1)
            browser.showPlaylist(set);
        else
            browser.showFolders();
        CHECK(browser.createComponentSnapshot(browser.getLocalBounds()).getWidth() == 1200);
    }
}

TEST_CASE("Library browser: refreshes after library changes", "[ui][library]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TestLibrary lib("refresh");
    const auto first = lib.add("First", 120.0);
    ui::LibraryBrowser browser(lib.db.get(), juce::File());
    browser.setBounds(0, 0, 1000, 300);
    browser.moveSelection(1);
    REQUIRE(browser.selectedTrackId() == first);

    // A write on the writer thread (an import, say): the browser hears about it and re-reads.
    lib.add("Second", 124.0);
    lib.add("Third", 126.0);
    browser.handlePendingLibraryChanges(); // what the message loop does
    CHECK(browser.model()->size() == 3);
    CHECK(browser.table().getNumRows() == 3);
    CHECK(browser.selectedTrackId() == first); // the selection stays on the same track

    // A new playlist shows up in the tree.
    lib.playlist("New set", {first});
    browser.handlePendingLibraryChanges();
    CHECK(browser.treeRowNames().contains("New set"));

    // A deleted track disappears.
    lib.db->writeAndWait([&](library::Connection& c) { CHECK(library::TrackRepository(c).remove(first)); });
    browser.handlePendingLibraryChanges();
    CHECK(browser.model()->size() == 2);
}

TEST_CASE("Library browser: the column layout persists (order, widths, hidden columns, sort)", "[ui][library]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TestLibrary lib("columns");
    lib.add("b track", 128.0);
    lib.add("a track", 120.0);
    const juce::TemporaryFile settingsFile(".xml");

    juce::String saved;
    std::vector<int> expectedColumns;
    {
        app::Settings settings(settingsFile.getFile());
        ui::LibraryBrowser browser(lib.db.get(), juce::File());
        browser.setBounds(0, 0, 1200, 300);
        browser.onColumnLayoutChanged = [&](const juce::String& layout)
        { settings.set(app::keys::libraryColumns, layout); };

        auto& header = browser.table().getHeader();
        CHECK_FALSE(header.isColumnVisible(static_cast<int>(TrackColumn::Album))); // hidden by default
        header.setColumnVisible(static_cast<int>(TrackColumn::Comment), false);
        header.setColumnVisible(static_cast<int>(TrackColumn::Album), true);
        header.moveColumn(static_cast<int>(TrackColumn::Bpm), 1); // BPM right after #
        header.setColumnWidth(static_cast<int>(TrackColumn::Title), 333);
        header.setSortColumnId(static_cast<int>(TrackColumn::Title), true);
        browser.syncWithHeader(); // what the header's async notification does in the app
        CHECK(browser.model()->sortColumn() == TrackColumn::Title);
        CHECK(browser.model()->row(0)->title == "a track");
        expectedColumns = visibleColumnIds(header);
        REQUIRE(settings.save());
        saved = settings.get(app::keys::libraryColumns);
    }
    REQUIRE(saved.isNotEmpty());

    // Next start: the app reads the setting and restores it.
    app::Settings settings(settingsFile.getFile());
    CHECK(settings.get(app::keys::libraryColumns) == saved);
    ui::LibraryBrowser browser(lib.db.get(), juce::File());
    browser.setBounds(0, 0, 1200, 300);
    browser.setColumnLayout(settings.get(app::keys::libraryColumns));
    auto& header = browser.table().getHeader();
    CHECK(visibleColumnIds(header) == expectedColumns);
    CHECK(expectedColumns[1] == static_cast<int>(TrackColumn::Bpm));
    CHECK_FALSE(header.isColumnVisible(static_cast<int>(TrackColumn::Comment)));
    CHECK(header.isColumnVisible(static_cast<int>(TrackColumn::Album)));
    CHECK(header.getColumnWidth(static_cast<int>(TrackColumn::Title)) == 333);
    CHECK(header.getSortColumnId() == static_cast<int>(TrackColumn::Title));
    CHECK(browser.model()->sortColumn() == TrackColumn::Title); // and the rows are sorted that way
    CHECK(browser.model()->row(0)->title == "a track");
}

TEST_CASE("Library browser: rows dragged onto a deck load there", "[ui][library]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    engine::AudioEngine engine;
    ui::DeckView deck(engine, 0, [](engine::BufferId) { return nullptr; });
    juce::File dropped;
    deck.onTrackDropped = [&](const juce::File& f) { dropped = f; };
    juce::Component source;
    const juce::DragAndDropTarget::SourceDetails track(juce::var("C:\\Music\\Night Drive.mp3"), &source, {});
    const juce::DragAndDropTarget::SourceDetails text(juce::var("not a path"), &source, {});
    const juce::DragAndDropTarget::SourceDetails notAudio(juce::var("C:\\Music\\cover.jpg"), &source, {});
    CHECK(deck.isInterestedInDragSource(track));
    CHECK_FALSE(deck.isInterestedInDragSource(text));
    CHECK_FALSE(deck.isInterestedInDragSource(notAudio));
    deck.itemDropped(track);
    CHECK(dropped.getFileName() == "Night Drive.mp3");
}

TEST_CASE("Library browser: without a database only Folders works", "[ui][library]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    ui::LibraryBrowser browser(nullptr, juce::File(), "disk full");
    browser.setBounds(0, 0, 800, 300);
    CHECK(browser.view() == ui::LibraryBrowser::View::Folders);
    CHECK(browser.status().contains("disk full"));
    CHECK(browser.model() == nullptr);
    browser.showCollection();
    CHECK(browser.view() == ui::LibraryBrowser::View::Folders);
}

// --- 50,000 rows at 60 fps ------------------------------------------------------------------------------------------

TEST_CASE("Library browser: scrolling 50,000 tracks stays inside a 60 fps frame", "[ui][library][perf]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TestLibrary lib("scroll");
    lib.fill(50000);
    ui::LibraryBrowser browser(lib.db.get(), juce::File());
    browser.setBounds(0, 0, 1416, 420); // the browser's size in a 1440 x 900 window
    browser.setColumnLayout({});
    REQUIRE(browser.table().getNumRows() == 50000);
    browser.table().getHeader().setSortColumnId(static_cast<int>(TrackColumn::Title), true);
    browser.syncWithHeader();

    juce::Image frame(juce::Image::RGB, browser.getWidth(), browser.getHeight(), false);
    auto* viewport = browser.table().getViewport();
    const int rowHeight = browser.table().getRowHeight();
    std::vector<double> ms;
    const auto paintFrame = [&](int y)
    {
        const auto start = std::chrono::steady_clock::now();
        viewport->setViewPosition(0, y); // what a wheel / scroll-bar move does
        juce::Graphics g(frame);
        browser.paintEntireComponent(g, false);
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    };
    // Smooth wheel scrolling (3 rows per frame) from the top, then flinging the scroll bar across the whole list
    // (every frame lands on rows not read yet), then a jump to the end and back. Debug draws ~25x slower, so it
    // draws a tenth of the frames (ctest's 120 s limit).
#if JUCE_DEBUG
    constexpr int kSmooth = 40, kFling = 20;
#else
    constexpr int kSmooth = 400, kFling = 200;
#endif
    for (int f = 0; f < kSmooth; ++f)
        paintFrame(f * 3 * rowHeight);
    const int total = 50000 * rowHeight;
    for (int f = 0; f < kFling; ++f)
        paintFrame(static_cast<int>(static_cast<double>(f) / kFling * total));
    paintFrame(total);
    paintFrame(0);

    std::vector<double> sorted = ms;
    std::sort(sorted.begin(), sorted.end());
    const double mean = std::accumulate(ms.begin(), ms.end(), 0.0) / static_cast<double>(ms.size());
    const double p95 = sorted[sorted.size() * 95 / 100];
    const double worst = sorted.back();
    WARN("50k rows: " << ms.size() << " frames, mean " << mean << " ms, 95th percentile " << p95 << " ms, worst "
                      << worst << " ms, " << browser.model()->pageLoads() << " pages read");
#if JUCE_DEBUG
    // Debug JUCE drawing is ~25x slower (text rendering without optimization): the 60 fps check is for Release,
    // the build people play with. Here only that every frame drew.
    CHECK(ms.size() == static_cast<std::size_t>(kSmooth + kFling + 2));
#else
    CHECK(mean < 16.7 / 2); // with room for the rest of the frame (waveforms, decks)
    CHECK(p95 < 16.7);
#endif
}

// Hidden: writes docs/images/p2-04-library.png (made-up tracks, no personal data). Run by hand:
//   DevilDJTests.exe "Library browser screenshot"
TEST_CASE("Library browser screenshot", "[.screenshot]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    TestLibrary lib("shot");
    struct Demo
    {
        const char* title;
        const char* artist;
        double bpm;
        int key, seconds, rating;
        std::uint32_t colour;
        const char* genre;
    };
    const Demo demo[] = {
        {"Midnight Drive", "Neon Owl", 122.0, 21, 361, 4, 0xE03030, "Deep House"},
        {"Paper Lanterns", "Kite Theory", 124.0, 16, 402, 5, 0, "Melodic House"},
        {"Glass Ocean", "Neon Owl", 123.0, 7, 377, 3, 0x30A0E0, "Deep House"},
        {"Low Sun", "Marrow & Moss", 120.0, 14, 298, 0, 0, "Downtempo"},
        {"Signal Fire", "The Northern Arcs", 126.0, 2, 415, 4, 0xE0C030, "Tech House"},
        {"Copper Skies", "Kite Theory", 124.0, 23, 389, 3, 0, "Melodic House"},
        {"Afterglow", "Juno Vale", 118.0, 9, 334, 2, 0x50C050, "Nu Disco"},
        {"Static Bloom", "Marrow & Moss", 128.0, 18, 356, 5, 0xE03030, "Techno"},
        {"Velvet Hour", "Juno Vale", 116.0, 12, 312, 0, 0, "Nu Disco"},
        {"Northern Lights", "The Northern Arcs", 125.0, 0, 428, 4, 0, "Tech House"},
    };
    std::vector<std::int64_t> ids;
    lib.db->writeAndWait(
        [&](library::Connection& c)
        {
            for (const auto& d : demo)
            {
                library::Track t;
                t.path = std::string("C:\\Music\\") + d.title + ".mp3";
                t.contentHash = t.path;
                t.title = d.title;
                t.artist = d.artist;
                t.bpm = d.bpm;
                t.musicalKey = d.key;
                t.durationMs = d.seconds * 1000;
                t.rating = d.rating;
                if (d.colour != 0)
                    t.color = d.colour;
                t.genre = d.genre;
                t.dateAdded = 1763500000;
                ids.push_back(library::TrackRepository(c).add(t).value());
            }
        });
    const auto gigs = lib.playlist("Gigs", {}, std::nullopt, library::PlaylistKind::Folder);
    lib.playlist("Friday warm-up", {ids[3], ids[0], ids[2]}, gigs);
    lib.playlist("Peak time", {ids[4], ids[7]}, gigs);
    lib.playlist("Sunday", {ids[6], ids[8]});

    ui::LibraryBrowser browser(lib.db.get(), juce::File());
    browser.setBounds(0, 0, 1416, 330);
    browser.moveSelection(1);
    browser.moveSelection(1);
    browser.setStatus("Added 10 tracks (0.1 s)");
    browser.setBounds(0, 0, 1416, 331); // the tree lays out its rows on a resize (or on the message loop)
    browser.setBounds(0, 0, 1416, 330);
    const auto image = browser.createComponentSnapshot(browser.getLocalBounds());
    const auto out =
        juce::File(juce::String::fromUTF8(DEVILDJ_SOURCE_DIR)).getChildFile("docs/images/p2-04-library.png");
    out.deleteFile();
    juce::FileOutputStream stream(out);
    REQUIRE(stream.openedOk());
    juce::PNGImageFormat png;
    REQUIRE(png.writeImageToStream(image, stream));

    // P2-05: a search and the filter row.
    ui::TrackFilter f;
    f.minRating = 3;
    f.bpmMin = 122.0;
    f.bpmMax = 126.0;
    browser.setFiltersShown(true);
    browser.setFilter(f);
    browser.setStatus({});
    browser.setBounds(0, 0, 1416, 331);
    browser.setBounds(0, 0, 1416, 330);
    const auto searched = browser.createComponentSnapshot(browser.getLocalBounds());
    const auto out2 =
        juce::File(juce::String::fromUTF8(DEVILDJ_SOURCE_DIR)).getChildFile("docs/images/p2-05-search.png");
    out2.deleteFile();
    juce::FileOutputStream stream2(out2);
    REQUIRE(stream2.openedOk());
    REQUIRE(png.writeImageToStream(searched, stream2));
}
TEST_CASE("Library browser: a long list starts at row 1", "[ui][library]")
{
    // Regression: the browser is built before it has a size; scrolling "row 1 into view" at height 0 left the
    // list one row down (row 1 hidden above the top) once it was laid out.
    juce::ScopedJuceInitialiser_GUI gui;
    TestLibrary lib("top");
    for (int i = 0; i < 44; ++i)
        lib.add("T" + std::to_string(i), 120.0);
    ui::LibraryBrowser browser(lib.db.get(), juce::File());
    browser.setColumnLayout(browser.columnLayout());
    browser.setBounds(0, 0, 1350, 190);
    CHECK(browser.table().getViewport()->getViewPositionY() == 0);
    browser.showFolders();
    browser.showCollection();
    CHECK(browser.table().getViewport()->getViewPositionY() == 0);
}
