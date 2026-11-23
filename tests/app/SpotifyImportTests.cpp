// P4-21: Spotify playlist import. Done when: matching tests on fixture playlists (exact, featuring / remix variants,
// missing); manual import of one of the owner's playlists.

#include "app/PlaylistMatch.h"
#include "app/SpotifyApi.h"
#include "app/SpotifyImport.h"
#include "app/SpotifyImportWindow.h"
#include "library/Connection.h"
#include "library/Database.h"
#include "library/PlaylistRepository.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>

using namespace devildj;
using app::RemoteTrack;
using app::TrackMatch;
using Kind = app::TrackMatch::Kind;

namespace
{
std::vector<app::LibraryTrack> fixtureLibrary()
{
    return {
        {1, "Levels", "Avicii", "Levels", 202.0},
        {2, "Titanium", "David Guetta ft. Sia", "Nothing but the Beat", 245.0},
        {3, "Strobe", "deadmau5", "For Lack of a Better Name", 637.0},
        {4, "Strobe (Club Edit)", "deadmau5", "Strobe", 391.0},
        {5, "Intro", "The xx", "xx", 127.0},
        {6, "Intro", "The xx", "xx (live)", 300.0},
        {7, "Café del Mar", "Energy 52", "Cafe del Mar", 455.0},
        {8, "One More Time", "Daft Punk", "Discovery", 320.0},
        {9, "Sandstorm", "Darude", "Before the Storm", 225.0},
        {10, "Sandstorm", "Darude", "Before the Storm", 225.0}, // the same file twice in the library
    };
}

RemoteTrack remote(std::string title, std::vector<std::string> artists, double seconds, std::string isrc = {})
{
    RemoteTrack r;
    r.title = std::move(title);
    r.artists = std::move(artists);
    r.seconds = seconds;
    r.isrc = std::move(isrc);
    return r;
}
} // namespace

TEST_CASE("Playlist matching: titles split into base, version and featuring; names normalized", "[app][spotify][match]")
{
    auto t = app::splitTitle("Strobe (Club Edit)");
    CHECK(t.base == "strobe");
    CHECK(t.version == "club edit");
    t = app::splitTitle("One More Time - Radio Edit");
    CHECK(t.base == "one more time");
    CHECK(t.version.empty()); // a radio edit counts as the song
    t = app::splitTitle("Titanium (feat. Sia)");
    CHECK(t.base == "titanium");
    REQUIRE(t.featuring.size() == 1);
    CHECK(t.featuring[0] == "sia");
    t = app::splitTitle("Get Lucky ft. Pharrell Williams");
    CHECK(t.base == "get lucky");
    CHECK(t.featuring[0] == "pharrell williams");
    t = app::splitTitle("Bohemian Rhapsody - 2011 Remaster");
    CHECK(t.base == "bohemian rhapsody");
    CHECK(t.version.empty());
    t = app::splitTitle("Sun Is Shining (Original Mix)");
    CHECK(t.version.empty());
    t = app::splitTitle("Let Me Love You (Part 2)");
    CHECK(t.base == "let me love you part 2"); // not a version: part of the title
    CHECK(app::normalizeName("Café  del-Mar!") == "cafe del mar");
    CHECK(app::splitArtists("David Guetta ft. Sia") == std::vector<std::string>{"david guetta", "sia"});
    CHECK(app::splitArtists("Above & Beyond") == std::vector<std::string>{"above", "beyond"});
}

TEST_CASE("Playlist matching on a fixture playlist: exact, featuring, remix variants, durations, duplicates, missing",
          "[app][spotify][match]")
{
    const app::PlaylistMatcher m(fixtureLibrary());
    // Exact.
    auto r = m.match(remote("Levels", {"Avicii"}, 203.0));
    CHECK(r.kind == Kind::Matched);
    CHECK(r.trackId == 1);
    // Featuring written differently on each side.
    r = m.match(remote("Titanium (feat. Sia)", {"David Guetta", "Sia"}, 245.0));
    CHECK(r.kind == Kind::Matched);
    CHECK(r.trackId == 2);
    // The edit that is in the library: that one, not the original.
    r = m.match(remote("Strobe - Club Edit", {"deadmau5"}, 390.0));
    CHECK(r.kind == Kind::Matched);
    CHECK(r.trackId == 4);
    r = m.match(remote("Strobe", {"deadmau5"}, 637.0));
    CHECK(r.kind == Kind::Matched);
    CHECK(r.trackId == 3);
    // A remix the library hasn't got: never quietly the original - the owner is asked.
    r = m.match(remote("Strobe (Somebody Remix)", {"deadmau5", "Somebody"}, 420.0));
    CHECK(r.kind == Kind::Ambiguous);
    CHECK(r.candidates.size() >= 1);
    // The same title twice: the duration decides.
    r = m.match(remote("Intro", {"The xx"}, 128.0));
    CHECK(r.kind == Kind::Matched);
    CHECK(r.trackId == 5);
    // Accents and the radio edit.
    CHECK(m.match(remote("Cafe Del Mar", {"Energy 52"}, 455.0)).trackId == 7);
    CHECK(m.match(remote("One More Time - Radio Edit", {"Daft Punk"}, 321.0)).kind == Kind::Matched);
    // The same file in the library twice: ambiguous, both offered.
    r = m.match(remote("Sandstorm", {"Darude"}, 225.0));
    CHECK(r.kind == Kind::Ambiguous);
    CHECK(r.candidates.size() == 2);
    // Not there.
    CHECK(m.match(remote("Nobody Has This Song", {"Nobody"}, 200.0)).kind == Kind::Missing);
    CHECK(m.match(remote("Levels", {"Someone Else Entirely"}, 120.0)).kind != Kind::Matched); // another song of that name
}

TEST_CASE("Playlist matching: 500 tracks against 20,000 in a moment", "[app][spotify][match][perf]")
{
    std::vector<app::LibraryTrack> lib;
    for (int i = 0; i < 20000; ++i)
        lib.push_back({i + 1, "Track " + std::to_string(i) + " Words Here", "Artist " + std::to_string(i % 500), "", 180.0 + i % 60});
    const auto t0 = std::chrono::steady_clock::now();
    const app::PlaylistMatcher m(lib);
    int found = 0;
    for (int i = 0; i < 500; ++i)
        found += m.match(remote("Track " + std::to_string(i * 37) + " Words Here", {"Artist " + std::to_string((i * 37) % 500)}, 180.0 + (i * 37) % 60))
                     .kind == Kind::Matched;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    WARN("index + 500 matches: " << seconds << " s");
    CHECK(found == 500);
#ifdef NDEBUG
    CHECK(seconds < 5.0);
#endif
}

TEST_CASE("Spotify API: paging, track shapes, a 429 waited out, a 401 refreshed, errors", "[app][spotify]")
{
    int refreshes = 0;
    app::SpotifyApi api([&](bool force, juce::String*) {
        if (force)
            ++refreshes;
        return juce::String(force ? "T2" : "T1");
    },
                        "http://fake");
    std::vector<int> waits;
    api.sleepFor = [&](int s) { waits.push_back(s); };
    std::vector<juce::String> asked;
    int playlistCalls = 0, trackCalls = 0;
    api.get = [&](const juce::URL& url, const juce::String& bearer) -> app::SpotifyApi::Response
    {
        const auto u = url.toString(true);
        asked.push_back(u + " " + bearer);
        if (u.contains("/v1/me/playlists"))
        {
            if (++playlistCalls == 1)
                return {429, "", 2}; // wait 2 s first
            if (!u.contains("page=2"))
                return {200, R"({"items":[{"id":"p1","name":"Warm up","owner":{"display_name":"me"},"tracks":{"total":3}},
                                          {"id":"p2","name":"Peak","items":{"total":1}}],"next":"http://fake/v1/me/playlists?page=2"})"};
            return {200, R"({"items":[{"id":"p3","name":"After","tracks":{"total":0}}],"next":null})"};
        }
        if (u.contains("/v1/playlists/p1/tracks"))
        {
            if (++trackCalls == 1)
                return {401, R"({"error":{"status":401,"message":"The access token expired"}})"};
            return {200, R"({"items":[
                {"track":{"type":"track","name":"Levels","artists":[{"name":"Avicii"}],"album":{"name":"Levels"},"duration_ms":202000,"external_ids":{"isrc":"SE0000000001"}}},
                {"track":{"type":"episode","name":"A podcast"}},
                {"track":null},
                {"item":{"type":"track","name":"Strobe","artists":[{"name":"deadmau5"}],"duration_ms":637000}}
            ],"next":null})"};
        }
        return {500, R"({"error":{"status":500,"message":"Server error"}})"};
    };
    const auto lists = api.playlists();
    REQUIRE(lists);
    REQUIRE(lists.value().size() == 3);
    CHECK(lists.value()[0].name == "Warm up");
    CHECK(lists.value()[0].owner == "me");
    CHECK(lists.value()[0].tracks == 3);
    CHECK(lists.value()[1].tracks == 1);
    CHECK(waits == std::vector<int>{2});
    const auto tracks = api.tracks("p1");
    REQUIRE(tracks);
    REQUIRE(tracks.value().size() == 2); // the episode and the gap left out
    CHECK(tracks.value()[0].title == "Levels");
    CHECK(tracks.value()[0].artists == std::vector<std::string>{"Avicii"});
    CHECK(tracks.value()[0].seconds == 202.0);
    CHECK(tracks.value()[0].isrc == "SE0000000001");
    CHECK(tracks.value()[1].title == "Strobe");
    CHECK(refreshes == 1);
    CHECK(asked.back().endsWith(" T2")); // the retry used the new token
    const auto broken = api.tracks("p9");
    CHECK_FALSE(broken);
    CHECK(juce::String(broken.error()).contains("500"));
    // Signed out: a clear error, nothing sent.
    app::SpotifyApi none([](bool, juce::String* e) { *e = "not signed in"; return juce::String(); }, "http://fake");
    int sent = 0;
    none.get = [&](const juce::URL&, const juce::String&) { ++sent; return app::SpotifyApi::Response{}; };
    CHECK_FALSE(none.playlists());
    CHECK(sent == 0);
}

TEST_CASE("Spotify import into the library: the playlist made in order, a choice taken, names kept apart, the rest listed",
          "[app][spotify][library]")
{
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("devildj-import-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        auto db = std::move(library::Database::open(dir / "library.db")).value();
        db->writeAndWait(
            [&](library::Connection& c)
            {
                int i = 0;
                for (const auto& t : fixtureLibrary())
                {
                    auto* st = c.prepare("INSERT INTO tracks (id, path, content_hash, title, artist, album, duration_ms, date_added) VALUES (?, ?, ?, ?, ?, ?, ?, 0)");
                    st->bind(1, t.id).bind(2, "C:\\m\\" + std::to_string(++i) + ".mp3").bind(3, "h" + std::to_string(i)).bind(4, t.title)
                        .bind(5, t.artist).bind(6, t.album).bind(7, static_cast<std::int64_t>(t.seconds * 1000.0));
                    REQUIRE(st->run());
                    st->reset();
                }
            });
        auto reader = std::move(db->openReader()).value();
        const auto lib = app::loadLibraryTracks(*reader);
        REQUIRE(lib.size() == 10);
        const app::PlaylistMatcher m(lib);
        auto plan = app::planImport("Warm up", {remote("One More Time", {"Daft Punk"}, 320.0), remote("Sandstorm", {"Darude"}, 225.0),
                                                remote("Missing One", {"Nobody"}, 100.0, "XX1"), remote("Levels", {"Avicii"}, 202.0)},
                                    m);
        CHECK(plan.matched() == 2);
        CHECK(plan.ambiguous() == 1);
        CHECK(plan.missing() == 1);
        plan.chosen[1] = 10; // the owner picks the second copy
        std::int64_t id = 0;
        db->writeAndWait([&](library::Connection& c) { id = app::createImportedPlaylist(c, plan).value(); });
        library::PlaylistRepository playlists(*reader);
        CHECK(playlists.get(id)->name == "Warm up (Spotify)");
        CHECK(playlists.trackIds(id) == std::vector<std::int64_t>{8, 10, 1}); // in the playlist's order, the missing one left out
        CHECK(app::missingReport(plan) == "Nobody - Missing One [XX1]\n");
        // Imported again: its own name.
        std::int64_t again = 0;
        db->writeAndWait([&](library::Connection& c) { again = app::createImportedPlaylist(c, plan).value(); });
        CHECK(playlists.get(again)->name == "Warm up (Spotify 2)");
        // The window's last steps: playlists made, the missing ones listed per playlist.
        juce::ScopedJuceInitialiser_GUI gui;
        int made = 0;
        app::SpotifyImportWindow::Source src;
        src.create = [&](const app::ImportPlan&) { ++made; return util::Result<std::int64_t>::ok(1); };
        app::SpotifyImportWindow w(src);
        w.plansToEdit().push_back(plan);
        CHECK(w.createPlaylists() == 1);
        CHECK(made == 1);
        CHECK(w.allMissing() == "# Warm up\nNobody - Missing One [XX1]\n");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}
