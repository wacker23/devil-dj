// P2-05: search and filter. Done when: search results < 50 ms on 50k tracks.

#include "library/Connection.h"
#include "library/Database.h"
#include "library/PlaylistRepository.h"
#include "library/TagRepository.h"
#include "library/TrackRepository.h"
#include "ui/LibraryBrowser.h"
#include "ui/TrackTableModel.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <vector>

using namespace devildj;
using ui::TrackFilter;
namespace fs = std::filesystem;

namespace
{
std::string utf8(const char8_t* text)
{
    return reinterpret_cast<const char*>(text);
}

struct Lib
{
    fs::path dir;
    std::unique_ptr<library::Database> db;
    std::unique_ptr<library::Connection> reader;

    explicit Lib(const char* name)
    {
        dir = fs::temp_directory_path() /
              (std::string("devildj-search-") + name + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        auto opened = library::Database::open(dir / "library.db");
        REQUIRE(opened.isOk());
        db = std::move(opened).value();
        reader = std::move(db->openReader()).value();
    }
    ~Lib()
    {
        reader.reset();
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    struct T
    {
        std::string title, artist, album, comment, genre;
        std::optional<double> bpm;
        std::optional<int> key;
        int rating = 0;
        std::optional<std::uint32_t> colour;
    };

    std::vector<std::int64_t> add(const std::vector<T>& tracks)
    {
        std::vector<std::int64_t> ids;
        db->writeAndWait(
            [&](library::Connection& c)
            {
                for (const auto& t : tracks)
                {
                    library::Track r;
                    r.path = "C:\\Music\\" + std::to_string(ids.size()) + ".mp3";
                    r.contentHash = r.path;
                    r.title = t.title;
                    r.artist = t.artist;
                    r.album = t.album;
                    r.comment = t.comment;
                    r.genre = t.genre;
                    r.bpm = t.bpm;
                    r.musicalKey = t.key;
                    r.rating = t.rating;
                    r.color = t.colour;
                    ids.push_back(library::TrackRepository(c).add(r).value());
                }
            });
        return ids;
    }

    void fill(int count)
    {
        db->writeAndWait(
            [&](library::Connection& c)
            {
                CHECK(c.exec("WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < " +
                             std::to_string(count) +
                             ") INSERT INTO tracks (path, content_hash, title, artist, album, genre, bpm, musical_key, "
                             "rating, color, date_added, comment) SELECT 'C:\\Music\\' || i || '.mp3', 'h' || i, "
                             "'Track ' || printf('%05d', i), 'Artist ' || (i % 500), 'Album ' || (i % 2000), "
                             "'Genre ' || (i % 20), 90 + (i % 80), i % 24, i % 6, "
                             "CASE WHEN i % 7 = 0 THEN 14692400 ELSE NULL END, 1763400000 + i, 'comment ' || i FROM n"));
            });
    }
};

std::set<std::int64_t> idsOf(ui::TrackTableModel& m)
{
    std::set<std::int64_t> out;
    for (int i = 0; i < m.size(); ++i)
        out.insert(m.trackIdAt(i));
    return out;
}

TrackFilter text(const char* t)
{
    TrackFilter f;
    f.text = juce::String::fromUTF8(t);
    return f;
}
} // namespace

TEST_CASE("Search: the full-text query", "[ui][search]")
{
    using M = ui::TrackTableModel;
    CHECK(M::ftsQuery("mid dri") == "\"mid\"* \"dri\"*");
    CHECK(M::ftsQuery("  Neon   Owl ") == "\"Neon\"* \"Owl\"*");
    CHECK(M::ftsQuery("say \"hi\"") == "\"say\"* \"hi\"*"); // quotes can't break the query
    CHECK(M::ftsQuery("a* OR b") == "\"a\"* \"OR\"* \"b\"*"); // FTS5 syntax is just text
    CHECK(M::ftsQuery("- & ( )") == "");
    CHECK(M::ftsQuery("") == "");
    CHECK(M::ftsQuery("warm-up") == "\"warm-up\"*"); // a phrase: "warm" then "up"
}

TEST_CASE("Search and filters narrow the track list", "[ui][search]")
{
    Lib lib("filters");
    const auto ids = lib.add({
        {"Midnight Drive", "Neon Owl", "After Hours", "warm-up opener", "Deep House", 122.0, 21, 4, 0xE03030},
        {"Paper Lanterns", "Kite Theory", "Lanterns EP", "", "Melodic House", 124.0, 16, 5, std::nullopt},
        {"Glass Ocean", "Neon Owl", "After Hours", "peak", "deep house", 123.0, 7, 3, 0x30A0E0},
        {utf8(u8"한밤의 드라이브"), utf8(u8"김서연"), "", "", "K-House", 126.0, 21, 0, std::nullopt},
        {"Untitled", "", "", "", "", std::nullopt, std::nullopt, 0, std::nullopt},
    });
    ui::TrackTableModel m(*lib.reader);
    CHECK(m.size() == 5);

    const auto only = [&](std::initializer_list<int> which)
    {
        std::set<std::int64_t> s;
        for (const int i : which)
            s.insert(ids[static_cast<std::size_t>(i)]);
        return s;
    };

    // Text: any of title / artist / album / comment, word starts, every word, ignoring case.
    m.setFilter(text("mid"));
    CHECK(idsOf(m) == only({0}));
    m.setFilter(text("neon"));
    CHECK(idsOf(m) == only({0, 2}));
    m.setFilter(text("neon gl"));
    CHECK(idsOf(m) == only({2}));
    m.setFilter(text("AFTER"));
    CHECK(idsOf(m) == only({0, 2}));
    m.setFilter(text("opener")); // comment
    CHECK(idsOf(m) == only({0}));
    m.setFilter(text("lantern")); // title and album of the same track
    CHECK(idsOf(m) == only({1}));
    m.setFilter(text("ight")); // not the start of a word
    CHECK(m.size() == 0);
    m.setFilter(text(reinterpret_cast<const char*>(u8"김서"))); // Korean
    CHECK(idsOf(m) == only({3}));
    m.setFilter(text(reinterpret_cast<const char*>(u8"드라")));
    CHECK(idsOf(m) == only({3}));
    m.setFilter(text("\"(*"));
    CHECK(m.size() == 5); // nothing searchable in it: no filter
    CHECK(m.error().isEmpty());

    TrackFilter f;
    f.bpmMin = 123.0;
    f.bpmMax = 124.0;
    m.setFilter(f);
    CHECK(idsOf(m) == only({1, 2})); // inclusive; no BPM never matches a range
    f = {};
    f.keys = {21};
    m.setFilter(f);
    CHECK(idsOf(m) == only({0, 3}));
    f.keys = {21, 7};
    m.setFilter(f);
    CHECK(idsOf(m) == only({0, 2, 3}));
    f = {};
    f.minRating = 4;
    m.setFilter(f);
    CHECK(idsOf(m) == only({0, 1}));
    f = {};
    f.colours = {0x30A0E0};
    m.setFilter(f);
    CHECK(idsOf(m) == only({2}));
    f = {};
    f.genre = "Deep House";
    m.setFilter(f);
    CHECK(idsOf(m) == only({0, 2})); // ignoring case

    // Everything together: all must match.
    f = text("neon");
    f.bpmMin = 122.5;
    f.genre = "deep house";
    m.setFilter(f);
    CHECK(idsOf(m) == only({2}));
    CHECK(m.unfilteredSize() == 5);

    // Sorting still applies to the filtered rows.
    m.setFilter(text("neon"));
    m.setSort(ui::TrackColumn::Title, true);
    REQUIRE(m.size() == 2);
    CHECK(m.row(0)->title == "Glass Ocean");
    CHECK(m.row(1)->title == "Midnight Drive");

    m.setFilter({});
    CHECK(m.size() == 5);
}

TEST_CASE("Filters: tags (all of them) and inside a playlist", "[ui][search]")
{
    Lib lib("tags");
    const auto ids = lib.add({{"A", "x", "", "", ""}, {"B", "x", "", "", ""}, {"C", "y", "", "", ""}});
    std::int64_t warm = 0, peak = 0, playlist = 0;
    lib.db->writeAndWait(
        [&](library::Connection& c)
        {
            library::TagRepository tags(c);
            warm = tags.createTag("Warm-up", std::nullopt).value();
            peak = tags.createTag("Peak", std::nullopt).value();
            CHECK(tags.assign(ids[0], warm));
            CHECK(tags.assign(ids[0], peak));
            CHECK(tags.assign(ids[1], warm));
            library::PlaylistRepository playlists(c);
            playlist = playlists.create({0, std::nullopt, "Set", library::PlaylistKind::Playlist}).value();
            const std::int64_t order[] = {ids[2], ids[1], ids[0]};
            CHECK(playlists.setTracks(playlist, order));
        });
    ui::TrackTableModel m(*lib.reader);
    TrackFilter f;
    f.tagIds = {warm};
    m.setFilter(f);
    CHECK(idsOf(m) == std::set<std::int64_t>{ids[0], ids[1]});
    f.tagIds = {warm, peak};
    m.setFilter(f);
    CHECK(idsOf(m) == std::set<std::int64_t>{ids[0]});

    // In a playlist: the filter applies on top, in playlist order.
    m.setSource({ui::TrackSource::Kind::Playlist, playlist});
    f = text("x");
    m.setFilter(f);
    REQUIRE(m.size() == 2);
    CHECK(m.trackIdAt(0) == ids[1]);
    CHECK(m.trackIdAt(1) == ids[0]);
    CHECK(m.unfilteredSize() == 3);
}

TEST_CASE("Search: results in under 50 ms on 50,000 tracks", "[ui][search][perf]")
{
    Lib lib("speed");
    lib.fill(50000);
    ui::TrackTableModel m(*lib.reader);
    REQUIRE(m.size() == 50000);
    m.setSort(ui::TrackColumn::Title, true);

    struct Case
    {
        const char* name;
        TrackFilter filter;
        int expected; // -1: just more than 0
    };
    std::vector<Case> cases;
    cases.push_back({"one title", text("track 01234"), 1});
    cases.push_back({"two words", text("artist 49"), -1}); // "49..." in artist, album or comment
    cases.push_back({"comment", text("comment 4999"), 11});      // 4999, 49990..49999
    cases.push_back({"one letter (matches everything)", text("t"), 50000});
    cases.push_back({"no match", text("zebra"), 0});
    {
        TrackFilter f;
        f.bpmMin = 120.0;
        f.bpmMax = 130.0;
        cases.push_back({"BPM 120-130", f, 50000 / 80 * 11});
    }
    {
        TrackFilter f = text("artist");
        f.bpmMin = 120.0;
        f.bpmMax = 130.0;
        f.keys = {6, 14, 22}; // genre 10 + BPM 120 = i % 80 == 30: keys 6, 14 or 22
        f.minRating = 3;
        f.genre = "Genre 10";
        cases.push_back({"search + BPM + key + rating + genre", f, -1});
    }
    for (const auto& c : cases)
    {
        double best = 1e9, worst = 0.0;
        for (int run = 0; run < 3; ++run)
        {
            m.setFilter({});
            m.setFilter(c.filter);
            best = std::min(best, m.lastQueryMs());
            worst = std::max(worst, m.lastQueryMs());
        }
        WARN(c.name << ": " << m.size() << " tracks in " << worst << " ms (worst of 3)");
        INFO(c.name);
        CHECK(m.error().isEmpty());
        if (c.expected >= 0)
            CHECK(m.size() == c.expected);
        else
            CHECK(m.size() > 0);
#if JUCE_DEBUG
        CHECK(worst < 1000.0); // Debug SQLite and STL are ~5x slower: a sanity bound; the 50 ms goal is Release
#else
        CHECK(worst < 50.0);
#endif
    }
}

TEST_CASE("Browser: search box, filter row and the track count", "[ui][search]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    Lib lib("browser");
    const auto ids = lib.add({
        {"Midnight Drive", "Neon Owl", "", "", "Deep House", 122.0, 21, 4, 0xE03030},
        {"Paper Lanterns", "Kite Theory", "", "", "Melodic House", 124.0, 16, 5, std::nullopt},
        {"Glass Ocean", "Neon Owl", "", "", "Deep House", 123.0, 7, 3, 0x30A0E0},
    });
    lib.db->writeAndWait(
        [&](library::Connection& c)
        {
            library::TagRepository tags(c);
            const auto mood = tags.createCategory("Mood").value();
            CHECK(tags.assign(ids[1], tags.createTag("Dreamy", mood).value()));
        });
    ui::LibraryBrowser browser(lib.db.get(), juce::File());
    browser.setBounds(0, 0, 1300, 300);
    CHECK(browser.countText() == "3 tracks");

    // The filter choices come from the library.
    CHECK(browser.genreFilter().getNumItems() == 3); // Any, Deep House, Melodic House
    CHECK(browser.tagFilter().getItemText(1) == "Mood: Dreamy");
    CHECK(browser.keyFilter().getNumItems() == 25);
    CHECK(ui::LibraryBrowser::colourName(0xE03030) == "red");
    CHECK(ui::LibraryBrowser::colourName(0x30A0E0) == "blue");
    CHECK(ui::LibraryBrowser::colourName(0x50C050) == "green");

    // Typing (the box's text, read when the filter is applied).
    browser.searchBox().setText("neon", false);
    TrackFilter f = browser.filterFromControls();
    CHECK(f.text == "neon");
    browser.setFilter(f);
    CHECK(browser.model()->size() == 2);
    CHECK(browser.countText() == "2 of 3 tracks");

    // The filter row.
    CHECK_FALSE(browser.filtersShown());
    browser.setFiltersShown(true);
    CHECK(browser.genreFilter().isVisible());
    CHECK(browser.table().getY() > browser.searchBox().getBottom() + 26); // the row sits above the table
    browser.genreFilter().setSelectedItemIndex(1, juce::sendNotificationSync); // Deep House
    f = browser.filterFromControls();
    CHECK(f.genre == "Deep House");
    CHECK(f.text == "neon");
    browser.tagFilter().setSelectedItemIndex(1, juce::sendNotificationSync); // Mood: Dreamy
    CHECK(browser.model()->size() == 0); // neon + deep house + dreamy: nothing
    CHECK(browser.countText() == "0 of 3 tracks");

    browser.clearFilters();
    CHECK(browser.model()->size() == 3);
    CHECK(browser.searchBox().getText().isEmpty());
    CHECK(browser.genreFilter().getSelectedId() == 1);
    CHECK(browser.countText() == "3 tracks");

    // Setting a filter sets the controls too.
    TrackFilter g;
    g.bpmMin = 123.0;
    g.keys = {16};
    browser.setFilter(g);
    CHECK(browser.filterFromControls() == g);
    CHECK(browser.model()->size() == 1);

    // The selection survives narrowing when the track is still there.
    browser.clearFilters();
    browser.selectTrack(ids[2]);
    browser.setFilter(text("glass"));
    CHECK(browser.selectedTrackId() == ids[2]);

    // Folders: no search box, no filter row.
    browser.showFolders();
    CHECK_FALSE(browser.searchBox().isVisible());
    CHECK_FALSE(browser.genreFilter().isVisible());
    browser.showCollection();
    CHECK(browser.searchBox().isVisible());
    CHECK(browser.genreFilter().isVisible()); // still shown: it was on
}
