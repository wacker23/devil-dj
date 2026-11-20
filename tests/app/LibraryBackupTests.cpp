// P2-11: library backup. Done when: restore test.

#include "LibraryBackup.h"
#include "Settings.h"
#include "library/Connection.h"
#include "library/CueRepository.h"
#include "library/Database.h"
#include "library/Migrations.h"
#include "library/PlaylistRepository.h"
#include "library/TrackRepository.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>

using namespace devildj;
namespace fs = std::filesystem;
namespace backup = app::backup;

namespace
{
const app::SettingKey<juce::String> kSomeSetting{"someSetting", {}};

struct Folder
{
    juce::File dir;
    Folder()
    {
        dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getChildFile("devildj-backup-" + juce::String(juce::Time::getHighResolutionTicks()));
        dir.createDirectory();
    }
    ~Folder() { dir.deleteRecursively(); }
};

fs::path toPath(const juce::File& f)
{
    return fs::path(f.getFullPathName().toWideCharPointer());
}

std::unique_ptr<library::Database> open(const juce::File& f)
{
    auto opened = library::Database::open(toPath(f));
    REQUIRE(opened.isOk());
    return std::move(opened).value();
}

struct Snapshot
{
    std::vector<std::string> titles;
    std::vector<std::int64_t> playlistTracks;
    std::string playlistName;
    std::size_t cues = 0;
};

Snapshot snapshot(library::Database& db)
{
    auto reader = std::move(db.openReader()).value();
    Snapshot s;
    for (const auto& t : library::TrackRepository(*reader).list())
        s.titles.push_back(t.title);
    const auto lists = library::PlaylistRepository(*reader).all();
    if (!lists.empty())
    {
        s.playlistName = lists[0].name;
        s.playlistTracks = library::PlaylistRepository(*reader).trackIds(lists[0].id);
    }
    s.cues = static_cast<std::size_t>(reader->queryInt("SELECT count(*) FROM cues").value_or(0));
    return s;
}
} // namespace

TEST_CASE("Backup: a zip that restores the library and the settings", "[app][backup]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    Folder tmp;
    const auto dbFile = tmp.dir.getChildFile("Devil-DJ/library.db");
    const auto settingsFile = tmp.dir.getChildFile("Devil-DJ/settings.xml");
    const auto zip = tmp.dir.getChildFile("backups/backup-20261120-101500.zip");

    // A library with tracks, a playlist and a cue, and a setting.
    Snapshot before;
    {
        auto db = open(dbFile);
        db->writeAndWait(
            [&](library::Connection& c)
            {
                library::TrackRepository tracks(c);
                std::vector<std::int64_t> ids;
                for (const char* title : {"Alpha", "Bravo", "Charlie"})
                {
                    library::Track t;
                    t.path = std::string("C:\\Music\\") + title + ".mp3";
                    t.contentHash = t.path;
                    t.title = title;
                    ids.push_back(tracks.add(t).value());
                }
                library::PlaylistRepository playlists(c);
                const auto p = playlists.create({0, std::nullopt, "Friday", library::PlaylistKind::Playlist}).value();
                const std::int64_t order[] = {ids[2], ids[0]};
                CHECK(playlists.setTracks(p, order));
                CHECK(library::CueRepository(c).add({0, ids[1], library::CueKind::HotCue, 0, 48000}).isOk());
            });
        {
            app::Settings settings(settingsFile);
            settings.set(kSomeSetting, juce::String("the original"));
            REQUIRE(settings.save());
        }
        before = snapshot(*db);
        REQUIRE(before.titles.size() == 3);

        const auto made = backup::create(*db, settingsFile, zip);
        REQUIRE(made.wasOk());
    }
    REQUIRE(zip.existsAsFile());
    CHECK_FALSE(zip.getSiblingFile(zip.getFileName() + ".partial").exists());
    {
        juce::ZipFile z(zip);
        CHECK(z.getEntry("library.db") != nullptr);
        CHECK(z.getEntry("settings.xml") != nullptr);
        REQUIRE(z.getEntry("manifest.txt") != nullptr);
        std::unique_ptr<juce::InputStream> m(z.createStreamForEntry(*z.getEntry("manifest.txt")));
        CHECK(m->readEntireStreamAsString().contains("library version: " +
                                                     juce::String(devildj::library::latestSchemaVersion())));
    }

    // Afterwards the library and the settings change.
    {
        auto db = open(dbFile);
        db->writeAndWait(
            [&](library::Connection& c)
            {
                library::TrackRepository tracks(c);
                CHECK(tracks.remove(tracks.list().front().id));
                library::Track t;
                t.path = "C:\\Music\\New.mp3";
                t.contentHash = t.path;
                t.title = "New";
                CHECK(tracks.add(t).isOk());
                library::PlaylistRepository playlists(c);
                auto p = playlists.all().front();
                p.name = "Renamed";
                CHECK(playlists.update(p));
            });
        app::Settings settings(settingsFile);
        settings.set(kSomeSetting, juce::String("changed"));
        REQUIRE(settings.save());
        CHECK_FALSE(snapshot(*db).titles == before.titles);
    } // the app closes the library

    // Restore: staged (checked), then swapped in when the app starts again.
    CHECK_FALSE(backup::restorePending(dbFile));
    const auto staged = backup::stageRestore(zip, dbFile, settingsFile);
    REQUIRE(staged.wasOk());
    CHECK(backup::restorePending(dbFile));
    // A stale journal next to the live file must not survive the swap (it belongs to the old library).
    dbFile.getSiblingFile("library.db-wal").replaceWithText("stale");
    const auto note = backup::applyPendingRestore(dbFile, settingsFile);
    CHECK(note.contains("restored"));
    CHECK(note.contains("settings restored"));
    CHECK_FALSE(backup::restorePending(dbFile));
    CHECK_FALSE(dbFile.getSiblingFile("library.db-wal").existsAsFile());
    CHECK(dbFile.getSiblingFile("library.db.before-restore").existsAsFile());
    CHECK(settingsFile.getSiblingFile("settings.xml.before-restore").existsAsFile());

    auto db = open(dbFile);
    const auto after = snapshot(*db);
    CHECK(after.titles == before.titles);
    CHECK(after.playlistName == "Friday");
    CHECK(after.playlistTracks == before.playlistTracks);
    CHECK(after.cues == 1);
    app::Settings settings(settingsFile);
    CHECK(settings.get(kSomeSetting) == "the original");

    // Nothing staged: starting again changes nothing.
    CHECK(backup::applyPendingRestore(dbFile, settingsFile).isEmpty());
}

TEST_CASE("Backup: a file that isn't a good backup is refused and nothing is staged", "[app][backup]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    Folder tmp;
    const auto dbFile = tmp.dir.getChildFile("library.db");
    const auto settingsFile = tmp.dir.getChildFile("settings.xml");

    const auto zipWith = [&](const juce::String& name, const juce::String& entry, const juce::File& content)
    {
        const auto zip = tmp.dir.getChildFile(name);
        juce::ZipFile::Builder b;
        b.addFile(content, 9, entry);
        juce::FileOutputStream out(zip);
        REQUIRE(b.writeToStream(out, nullptr));
        return zip;
    };
    const auto notes = tmp.dir.getChildFile("notes.txt");
    notes.replaceWithText("hello");
    const auto broken = tmp.dir.getChildFile("broken.db");
    broken.replaceWithText("this is not a database at all, just some text that is long enough to look like one");
    const auto newer = tmp.dir.getChildFile("newer.db");
    {
        auto c = std::move(library::Connection::open(toPath(newer), library::Connection::Mode::ReadWrite)).value();
        REQUIRE(c->exec("CREATE TABLE x (a); PRAGMA user_version = 99"));
    }

    const auto plain = tmp.dir.getChildFile("plain.zip");
    plain.replaceWithText("not a zip");
    for (const auto& zip : {plain, zipWith("no-db.zip", "notes.txt", notes), zipWith("broken.zip", "library.db", broken),
                            zipWith("newer.zip", "library.db", newer)})
    {
        INFO(zip.getFileName());
        const auto r = backup::stageRestore(zip, dbFile, settingsFile);
        CHECK(r.failed());
        CHECK_FALSE(backup::restorePending(dbFile));
    }
    CHECK(backup::stageRestore(tmp.dir.getChildFile("newer.zip"), dbFile, settingsFile)
              .getErrorMessage()
              .contains("newer Devil-DJ"));
}

TEST_CASE("Backup: once a day automatically, the newest 7 kept", "[app][backup]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    Folder tmp;
    const auto folder = tmp.dir.getChildFile("backups");
    folder.createDirectory();
    const juce::Time now(2026, 10, 20, 12, 0, 0, 0, true); // 20 Nov 2026 12:00

    CHECK(backup::automaticDue(folder, now)); // none yet

    // Ten days of automatic backups and a hand-made one.
    for (int day = 1; day <= 10; ++day)
        folder.getChildFile("auto-202611" + juce::String(day).paddedLeft('0', 2) + "-090000.zip").replaceWithText("x");
    folder.getChildFile("backup-20261101-080000.zip").replaceWithText("mine");
    const auto all = backup::list(folder);
    REQUIRE(all.size() == 11);
    CHECK(all.front().file.getFileName() == "auto-20261110-090000.zip"); // newest first
    CHECK_FALSE(all.back().automatic);

    CHECK(backup::automaticDue(folder, now)); // the newest is 10 days old
    CHECK_FALSE(backup::automaticDue(folder, juce::Time(2026, 10, 10, 20, 0, 0, 0, true))); // 11 hours later
    backup::pruneAutomatic(folder);
    int autos = 0;
    for (const auto& b : backup::list(folder))
        autos += b.automatic ? 1 : 0;
    CHECK(autos == 7);
    CHECK_FALSE(folder.getChildFile("auto-20261103-090000.zip").exists()); // the oldest went
    CHECK(folder.getChildFile("auto-20261104-090000.zip").exists());
    CHECK(folder.getChildFile("backup-20261101-080000.zip").exists()); // never pruned

    // runAutomatic: makes one when due (and prunes), none when not.
    auto db = open(tmp.dir.getChildFile("library.db"));
    juce::String error;
    const auto zip = backup::runAutomatic(*db, tmp.dir.getChildFile("settings.xml"), folder, now, &error);
    CHECK(error.isEmpty());
    REQUIRE(zip.existsAsFile());
    CHECK(zip.getFileName() == "auto-20261120-120000.zip");
    CHECK(juce::ZipFile(zip).getEntry("library.db") != nullptr);
    autos = 0;
    for (const auto& b : backup::list(folder))
        autos += b.automatic ? 1 : 0;
    CHECK(autos == 7);
    CHECK(backup::runAutomatic(*db, {}, folder, now + juce::RelativeTime::hours(1)) == juce::File());
}
