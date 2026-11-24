// P2-01: library database layer - connection + statement cache, migrations, WAL writer thread and readers.

#include "library/Connection.h"
#include "library/Database.h"
#include "library/Migrations.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace devildj::library;
namespace fs = std::filesystem;

namespace
{
// Assertions inside database jobs use CHECK, not REQUIRE: a REQUIRE failing on the writer thread would
// throw there.

// A fresh folder per test, removed afterwards. The name has Korean in it: user folders often do.
struct TempFolder
{
    fs::path dir;
    explicit TempFolder(const char* name)
    {
        dir = fs::temp_directory_path() / (std::string("devildj-db-") + name + "-" +
                                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        dir /= fs::path(u8"라이브러리");
        fs::create_directories(dir);
    }
    ~TempFolder()
    {
        std::error_code ec;
        fs::remove_all(dir.parent_path(), ec);
    }
    fs::path file() const { return dir / "library.db"; }
};

bool insertTrack(Connection& c, const std::string& path, const std::string& title, const std::string& artist = {})
{
    auto* s = c.prepare("INSERT INTO tracks (path, content_hash, title, artist, date_added) VALUES (?, 'h', ?, ?, 0)");
    return s != nullptr && s->bind(1, path).bind(2, title).bind(3, artist).run();
}

std::int64_t count(Connection& c, const std::string& table)
{
    return c.queryInt("SELECT count(*) FROM " + table).value_or(-1);
}

std::set<std::string> tableNames(Connection& c)
{
    std::set<std::string> names;
    auto* s = c.prepare("SELECT name FROM sqlite_master WHERE type IN ('table', 'trigger')");
    while (s->step())
        names.insert(s->getText(0));
    return names;
}

std::int64_t ftsMatches(Connection& c, const std::string& query)
{
    auto* s = c.prepare("SELECT count(*) FROM tracks_fts WHERE tracks_fts MATCH ?");
    s->bind(1, query);
    const auto n = s->step() ? s->getInt(0) : -1;
    s->reset();
    return n;
}
} // namespace

TEST_CASE("Migrations: an empty database gets schema v1", "[library][db]")
{
    TempFolder tmp("empty");
    auto db = Database::open(tmp.file());
    REQUIRE(db.isOk());
    CHECK(db.value()->schemaVersion() == latestSchemaVersion());
    CHECK(latestSchemaVersion() == 5); // v2: the Track Matcher's tables (P2-24); v3: saved transitions (P4-19);
                                       // v4: phrases (P6-06); v5: smart cue answers (P6-07)

    auto reader = db.value()->openReader();
    REQUIRE(reader.isOk());
    auto& r = *reader.value();
    CHECK(schemaVersion(r) == latestSchemaVersion());
    CHECK(r.queryText("PRAGMA journal_mode").value_or("") == "wal");

    const auto names = tableNames(r);
    for (const char* t : {"tracks", "beatgrids", "cues", "playlists", "playlist_tracks", "tag_categories", "tags",
                          "track_tags", "history", "analysis_artifacts", "tracks_fts", "tracks_fts_insert",
                          "tracks_fts_delete", "tracks_fts_update"})
    {
        INFO(t);
        CHECK(names.count(t) == 1);
    }
    CHECK(count(r, "tracks") == 0);
}

TEST_CASE("Migrations: re-running is a no-op and the version survives reopening", "[library][db]")
{
    TempFolder tmp("reopen");
    {
        auto db = Database::open(tmp.file());
        REQUIRE(db.isOk());
        db.value()->writeAndWait([](Connection& c) { CHECK(insertTrack(c, "C:/a.mp3", "Alpha")); });
    }
    auto again = Database::open(tmp.file());
    REQUIRE(again.isOk());
    CHECK(again.value()->schemaVersion() == latestSchemaVersion());
    auto reader = again.value()->openReader();
    REQUIRE(reader.isOk());
    CHECK(count(*reader.value(), "tracks") == 1); // nothing dropped or re-created

    auto raw = Connection::open(tmp.file(), Connection::Mode::ReadWrite);
    REQUIRE(raw.isOk());
    auto result = migrate(*raw.value());
    REQUIRE(result.isOk());
    CHECK(result.value() == latestSchemaVersion());
}

TEST_CASE("Migrations: a database from a newer Devil-DJ is refused, not touched", "[library][db]")
{
    TempFolder tmp("newer");
    {
        auto raw = Connection::open(tmp.file(), Connection::Mode::ReadWrite);
        REQUIRE(raw.isOk());
        REQUIRE(raw.value()->exec("CREATE TABLE future (x); PRAGMA user_version = 99"));
    }
    auto db = Database::open(tmp.file());
    REQUIRE_FALSE(db.isOk());
    CHECK(db.error().find("newer Devil-DJ") != std::string::npos);

    auto raw = Connection::open(tmp.file(), Connection::Mode::ReadOnly);
    REQUIRE(raw.isOk());
    CHECK(schemaVersion(*raw.value()) == 99);
    CHECK(tableNames(*raw.value()).count("tracks") == 0);
}

TEST_CASE("Schema v1: full-text search follows tracks; deletes cascade", "[library][db]")
{
    TempFolder tmp("fts");
    auto db = Database::open(tmp.file());
    REQUIRE(db.isOk());
    db.value()->writeAndWait(
        [](Connection& c)
        {
            CHECK(insertTrack(c, "C:/1.mp3", "Midnight Drive", "Neon Owl"));
            CHECK(insertTrack(c, "C:/2.mp3", "Morning Light", "Paper Fox"));
            CHECK(ftsMatches(c, "midnight") == 1);
            CHECK(ftsMatches(c, "owl") == 1);
            CHECK(ftsMatches(c, "m*") == 2);

            CHECK(c.exec("UPDATE tracks SET title = 'Evening Light' WHERE path = 'C:/1.mp3'"));
            CHECK(ftsMatches(c, "midnight") == 0);
            CHECK(ftsMatches(c, "light") == 2);

            // Cues, beatgrid and playlist entries go with their track (foreign keys are on).
            CHECK(c.exec("INSERT INTO cues (track_id, kind, slot, start_sample) VALUES (1, 0, 0, 100);"
                           "INSERT INTO beatgrids (track_id, anchors_json) VALUES (1, '[]');"
                           "INSERT INTO playlists (name, kind, created_at) VALUES ('Set', 1, 0);"
                           "INSERT INTO playlist_tracks VALUES (1, 1, 0), (1, 2, 1);"));
            CHECK(c.exec("DELETE FROM tracks WHERE id = 1"));
            CHECK(count(c, "cues") == 0);
            CHECK(count(c, "beatgrids") == 0);
            CHECK(count(c, "playlist_tracks") == 1);
            CHECK(ftsMatches(c, "evening") == 0);
            CHECK(ftsMatches(c, "morning") == 1);

            // A cue for a track that doesn't exist is rejected.
            CHECK_FALSE(c.exec("INSERT INTO cues (track_id, kind, start_sample) VALUES (42, 0, 0)"));
            CHECK(c.lastError().find("FOREIGN KEY") != std::string::npos);
        });
}

TEST_CASE("Connection: statements are prepared once and reused", "[library][db]")
{
    TempFolder tmp("cache");
    auto db = Database::open(tmp.file());
    REQUIRE(db.isOk());
    db.value()->writeAndWait(
        [](Connection& c)
        {
            const std::string sql = "INSERT INTO tracks (path, content_hash, date_added) VALUES (?, 'h', 0)";
            auto* first = c.prepare(sql);
            CHECK(first != nullptr);
            if (first == nullptr)
                return;
            const auto cached = c.cachedStatementCount();
            for (int i = 0; i < 50; ++i)
            {
                auto* s = c.prepare(sql);
                CHECK(s == first);
                CHECK(s->bind(1, "C:/" + std::to_string(i) + ".mp3").run());
            }
            CHECK(c.cachedStatementCount() == cached);
            CHECK(count(c, "tracks") == 50);

            // Errors are reported, not thrown, and the statement stays usable.
            auto* dup = c.prepare(sql);
            CHECK_FALSE(dup->bind(1, "C:/0.mp3").run());
            CHECK(dup->error().find("UNIQUE") != std::string::npos);
            CHECK(c.prepare(sql) == dup); // prepare() resets it, error included
            CHECK(dup->ok());
            CHECK(dup->bind(1, "C:/new.mp3").run());

            CHECK(c.prepare("SELEC nonsense") == nullptr);
            CHECK_FALSE(c.lastError().empty());

            // Optional values bind as NULL.
            auto* upd = c.prepare("UPDATE tracks SET bpm = ?, year = ? WHERE path = 'C:/1.mp3'");
            CHECK(upd->bind(1, std::optional<double>{}).bind(2, std::optional<std::int64_t>{2024}).run());
            auto* get = c.prepare("SELECT bpm, year FROM tracks WHERE path = 'C:/1.mp3'");
            CHECK(get->step());
            CHECK_FALSE(get->getOptionalDouble(0).has_value());
            CHECK(get->getOptionalInt(1) == 2024);
            get->reset();
        });
}

TEST_CASE("Connection: a transaction rolls back when its body fails", "[library][db]")
{
    TempFolder tmp("rollback");
    auto db = Database::open(tmp.file());
    REQUIRE(db.isOk());
    db.value()->writeAndWait(
        [](Connection& c)
        {
            const bool ok = c.transaction(
                [&]
                {
                    return insertTrack(c, "C:/a.mp3", "A") && insertTrack(c, "C:/b.mp3", "B") &&
                           insertTrack(c, "C:/a.mp3", "duplicate path");
                });
            CHECK_FALSE(ok);
            CHECK(count(c, "tracks") == 0);
            CHECK(c.transaction([&] { return insertTrack(c, "C:/a.mp3", "A"); }));
            CHECK(count(c, "tracks") == 1);
        });
}

TEST_CASE("Database: writes run in order on one thread, off the caller", "[library][db]")
{
    TempFolder tmp("order");
    auto db = Database::open(tmp.file());
    REQUIRE(db.isOk());
    auto& d = *db.value();

    std::vector<int> order;
    std::set<std::thread::id> threads;
    for (int i = 0; i < 100; ++i)
        d.write(
            [&, i](Connection& c)
            {
                order.push_back(i);
                threads.insert(std::this_thread::get_id());
                insertTrack(c, "C:/" + std::to_string(i) + ".mp3", "T");
            });
    d.flush();
    REQUIRE(order.size() == 100);
    for (int i = 0; i < 100; ++i)
        CHECK(order[static_cast<std::size_t>(i)] == i);
    CHECK(threads.size() == 1);
    CHECK(threads.count(std::this_thread::get_id()) == 0);

    // writeAndWait from inside a job runs in place (no deadlock).
    bool inner = false;
    d.writeAndWait([&](Connection&) { d.writeAndWait([&](Connection&) { inner = d.isWriterThread(); }); });
    CHECK(inner);

    // Queued writes still run when the database closes.
    TempFolder tmp2("close");
    {
        auto db2 = Database::open(tmp2.file());
        REQUIRE(db2.isOk());
        for (int i = 0; i < 20; ++i)
            db2.value()->write([i](Connection& c) { insertTrack(c, "C:/" + std::to_string(i), "T"); });
    }
    auto reopened = Database::open(tmp2.file());
    REQUIRE(reopened.isOk());
    auto reader = reopened.value()->openReader();
    REQUIRE(reader.isOk());
    CHECK(count(*reader.value(), "tracks") == 20);
}

TEST_CASE("Database: a reader reads while a write is in progress", "[library][db]")
{
    TempFolder tmp("concurrent");
    auto db = Database::open(tmp.file());
    REQUIRE(db.isOk());
    auto& d = *db.value();
    d.writeAndWait([](Connection& c) { CHECK(insertTrack(c, "C:/first.mp3", "First")); });

    auto opened = d.openReader();
    REQUIRE(opened.isOk());
    auto& reader = *opened.value();

    // The writer opens a transaction, writes 1000 rows and holds it open until the reader has looked.
    std::mutex m;
    std::condition_variable cv;
    bool written = false, looked = false;
    d.write(
        [&](Connection& c)
        {
            const bool ok = c.transaction(
                [&]
                {
                    for (int i = 0; i < 1000; ++i)
                        if (!insertTrack(c, "C:/bulk/" + std::to_string(i) + ".mp3", "Bulk"))
                            return false;
                    std::unique_lock lock(m);
                    written = true;
                    cv.notify_all();
                    return cv.wait_for(lock, std::chrono::seconds(10), [&] { return looked; });
                });
            CHECK(ok);
        });

    {
        std::unique_lock lock(m);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(10), [&] { return written; }));
    }
    // Mid-write: the reader isn't blocked and sees the last committed state.
    const auto start = std::chrono::steady_clock::now();
    const auto during = count(reader, "tracks");
    const auto readTime = std::chrono::steady_clock::now() - start;
    CHECK(during == 1);
    CHECK(reader.lastError().empty());
    CHECK(readTime < std::chrono::milliseconds(500)); // not a busy-wait on the writer's lock
    {
        std::lock_guard lock(m);
        looked = true;
    }
    cv.notify_all();
    d.flush();
    CHECK(count(reader, "tracks") == 1001);

    // A reader can't write.
    CHECK_FALSE(reader.exec("DELETE FROM tracks"));
    CHECK(count(reader, "tracks") == 1001);
}

TEST_CASE("Database: readers on several threads during a stream of writes", "[library][db]")
{
    TempFolder tmp("stress");
    auto db = Database::open(tmp.file());
    REQUIRE(db.isOk());
    auto& d = *db.value();

    std::atomic<bool> stop{false};
    std::atomic<int> errors{0}, reads{0}, wentBackwards{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t)
        readers.emplace_back(
            [&]
            {
                auto opened = d.openReader();
                if (!opened)
                {
                    ++errors;
                    return;
                }
                auto& r = *opened.value();
                std::int64_t last = 0;
                while (!stop)
                {
                    const auto n = r.queryInt("SELECT count(*) FROM tracks");
                    if (!n)
                    {
                        ++errors;
                        continue;
                    }
                    if (*n < last)
                        ++wentBackwards;
                    last = *n;
                    ++reads;
                }
            });

    // Let every reader get going first, so the writes really happen while they read (the writes alone can finish
    // before a reader thread has opened its connection).
    for (int wait = 0; wait < 1000 && reads < 3 * 3 && errors == 0; ++wait)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const int readsBeforeWrites = reads;

    for (int batch = 0; batch < 50; ++batch)
        d.write(
            [batch](Connection& c)
            {
                c.transaction(
                    [&]
                    {
                        for (int i = 0; i < 20; ++i)
                            if (!insertTrack(c, "C:/" + std::to_string(batch) + "/" + std::to_string(i), "T"))
                                return false;
                        return true;
                    });
            });
    d.flush();
    stop = true;
    for (auto& t : readers)
        t.join();

    CHECK(errors == 0);
    CHECK(wentBackwards == 0);
    CHECK(readsBeforeWrites > 0); // the readers were reading when the writes began
    auto reader = d.openReader();
    REQUIRE(reader.isOk());
    CHECK(count(*reader.value(), "tracks") == 1000);
}
