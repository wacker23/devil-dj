// P2-23: history. Done when: history survives restart; transitions queryable.

#include "PlayTracker.h"
#include "library/Connection.h"
#include "library/Database.h"
#include "library/HistoryRepository.h"
#include "library/PlaylistRepository.h"
#include "library/TrackRepository.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace devildj;
namespace fs = std::filesystem;

namespace
{
fs::path tempDir()
{
    return fs::temp_directory_path() /
           ("devildj-history-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

std::vector<std::int64_t> addTracks(library::Database& db, int n)
{
    std::vector<std::int64_t> ids;
    db.writeAndWait(
        [&](library::Connection& c)
        {
            for (int i = 0; i < n; ++i)
            {
                library::Track t;
                t.path = "C:\\Music\\track" + std::to_string(i) + ".mp3";
                t.contentHash = "h" + std::to_string(i);
                t.title = "Night Drive " + std::to_string(i);
                t.artist = i == 2 ? "" : "Artist " + std::to_string(i);
                ids.push_back(library::TrackRepository(c).add(t).value());
            }
        });
    return ids;
}
} // namespace

TEST_CASE("History: sessions, plays and transitions; it survives a restart", "[library][history]")
{
    const auto dir = tempDir();
    std::vector<std::int64_t> tracks;
    std::int64_t first = 0, second = 0;
    constexpr std::int64_t t0 = 1763669580; // a fixed time
    {
        auto db = std::move(library::Database::open(dir / "library.db")).value();
        tracks = addTracks(*db, 4);
        db->writeAndWait(
            [&](library::Connection& c)
            {
                library::HistoryRepository h(c);
                first = h.startSession(t0).value();
                // A -> B -> C, then B again.
                CHECK(h.recordPlay(first, tracks[0], t0 + 100, 0));
                CHECK(h.recordPlay(first, tracks[1], t0 + 400, 1));
                CHECK(h.recordPlay(first, tracks[2], t0 + 700, 0));
                CHECK(h.recordPlay(first, tracks[1], t0 + 1000, 1));
                // A second session: its first track has no transition into it.
                second = h.startSession(t0 + 86400).value();
                CHECK(h.recordPlay(second, tracks[0], t0 + 86500, 0));
                CHECK(h.recordPlay(second, tracks[1], t0 + 86800, 1));
                library::Playlist plain;
                plain.name = "Warm-up";
                const auto ordinary = library::PlaylistRepository(c).create(plain).value();
                CHECK_FALSE(h.recordPlay(ordinary, tracks[1], t0, 0)); // a playlist, not a session
                CHECK_FALSE(h.recordPlay(9999, tracks[1], t0, 0));
            });
    }

    // Next start: all of it is there.
    auto db = std::move(library::Database::open(dir / "library.db")).value();
    auto reader = std::move(db->openReader()).value();
    library::HistoryRepository h(*reader);
    CHECK(h.sessions() == std::vector<std::int64_t>{second, first}); // newest first
    const auto played = h.entries(first);
    REQUIRE(played.size() == 4);
    CHECK(played[0].trackId == tracks[0]);
    CHECK_FALSE(played[0].previousTrackId.has_value());
    CHECK(played[1].previousTrackId == tracks[0]);
    CHECK(played[3].previousTrackId == tracks[2]);
    CHECK(played[1].deck == 1);
    CHECK(played[2].playedAt == t0 + 700);
    // The session is a History playlist: its tracks in play order, its name the date and time.
    library::PlaylistRepository playlists(*reader);
    const auto session = playlists.get(first);
    REQUIRE(session.has_value());
    CHECK(session->kind == library::PlaylistKind::History);
    CHECK(session->name.size() == 16); // "YYYY-MM-DD HH:MM"
    CHECK(session->name.substr(0, 2) == "20");
    CHECK(playlists.trackIds(first) == std::vector<std::int64_t>{tracks[0], tracks[1], tracks[2], tracks[1]});

    // Transitions, queryable: all of them, a pair's count, what followed a track.
    const auto all = h.transitions();
    REQUIRE(all.size() == 4); // A->B, B->C, C->B, then A->B again (second session)
    CHECK(all.front().fromTrackId == tracks[0]);
    CHECK(all.front().toTrackId == tracks[1]);
    CHECK(h.transitionCount(tracks[0], tracks[1]) == 2);
    CHECK(h.transitionCount(tracks[1], tracks[2]) == 1);
    CHECK(h.transitionCount(tracks[1], tracks[0]) == 0);
    CHECK(h.playedAfter(tracks[0]) == std::vector<std::pair<std::int64_t, int>>{{tracks[1], 2}});
    CHECK(h.playedAfter(tracks[3]).empty());

    // The tracklist.
    const auto lines = h.tracklist(first);
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].rfind("01. ", 0) == 0);
    CHECK(lines[0].find("Artist 0 - Night Drive 0") != std::string::npos);
    CHECK(lines[2].find("  Night Drive 2") != std::string::npos); // no artist: no " - "
    const auto file = dir / "tracklist.txt";
    REQUIRE(h.exportTracklist(first, file).value() == 4);
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    CHECK(text.rfind("\xEF\xBB\xBF" + session->name + "\r\n\r\n01. ", 0) == 0);
    CHECK(text.find("04. ") != std::string::npos);
    CHECK_FALSE(h.exportTracklist(9999, dir / "nope.txt")); // no such session
    for (const auto& p : playlists.all())
        if (p.kind == library::PlaylistKind::Playlist)
            CHECK_FALSE(h.exportTracklist(p.id, dir / "nope.txt")); // an ordinary playlist

    reader.reset();
    db.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("History: a track counts as played after 60 s of playing, once per load", "[app][history]")
{
    app::PlayTracker t;
    t.loaded(0, 42);
    for (int i = 0; i < 59; ++i)
        CHECK_FALSE(t.update(0, true, 1.0).has_value());
    CHECK_FALSE(t.update(0, false, 30.0).has_value()); // paused: doesn't count
    CHECK(t.playedSeconds(0) == 59.0);
    CHECK(t.update(0, true, 1.0) == 42); // 60 s
    CHECK_FALSE(t.update(0, true, 100.0).has_value()); // once
    t.loaded(0, 42); // loaded again: counts again
    CHECK_FALSE(t.update(0, true, 30.0).has_value());
    CHECK(t.update(0, true, 30.0) == 42);
    // Decks count on their own; a file outside the library (id 0) never counts.
    t.loaded(1, 7);
    t.loaded(2, 0);
    CHECK_FALSE(t.update(1, true, 59.9).has_value());
    CHECK_FALSE(t.update(2, true, 600.0).has_value());
    CHECK(t.update(1, true, 0.2) == 7);
    CHECK_FALSE(t.update(9, true, 100.0).has_value());
}
