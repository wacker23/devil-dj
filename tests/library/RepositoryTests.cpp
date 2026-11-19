// P2-03: repositories - create / read / update / delete for tracks, playlists, cues and tags, plus the change
// notifications the UI listens to. Writes run as jobs on the database's writer thread (like the app); reads use a
// reader connection. Assertions inside jobs use CHECK (a REQUIRE would throw on the writer thread).

#include "library/Connection.h"
#include "library/CueRepository.h"
#include "library/Database.h"
#include "library/PlaylistRepository.h"
#include "library/TagRepository.h"
#include "library/TrackRepository.h"
#include "util/RealtimeGuard.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <vector>

using namespace devildj::library;
namespace fs = std::filesystem;

namespace
{
struct Lib
{
    fs::path dir;
    std::unique_ptr<Database> db;
    std::unique_ptr<Connection> reader;
    std::mutex changeMutex;
    std::vector<Change> changes;
    int listener = 0;

    explicit Lib(const char* name)
    {
        dir = fs::temp_directory_path() /
              (std::string("devildj-repo-") + name + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        auto opened = Database::open(dir / "library.db");
        REQUIRE(opened.isOk());
        db = std::move(opened).value();
        auto r = db->openReader();
        REQUIRE(r.isOk());
        reader = std::move(r).value();
        listener = db->addChangeListener(
            [this](const std::vector<Change>& c)
            {
                std::lock_guard lock(changeMutex);
                changes.insert(changes.end(), c.begin(), c.end());
            });
    }
    ~Lib()
    {
        db.reset();
        reader.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    template <typename F>
    void write(F&& f)
    {
        db->writeAndWait([&](Connection& c) { f(c); });
    }

    std::vector<Change> takeChanges()
    {
        std::lock_guard lock(changeMutex);
        return std::exchange(changes, {});
    }

    std::int64_t addTrack(const std::string& path)
    {
        std::int64_t id = 0;
        write(
            [&](Connection& c)
            {
                Track t;
                t.path = path;
                t.contentHash = "hash-" + path;
                auto r = TrackRepository(c).add(t);
                CHECK(r.isOk());
                if (r)
                    id = r.value();
            });
        takeChanges();
        return id;
    }
};

bool has(const std::vector<Change>& changes, Change c)
{
    return std::find(changes.begin(), changes.end(), c) != changes.end();
}

Track fullTrack()
{
    Track t;
    t.path = "C:\\Music\\Night Drive.mp3";
    t.contentHash = "0123456789abcdef";
    t.title = "Night Drive";
    t.artist = "Neon Owl";
    t.album = "After Hours";
    t.genre = "Deep House";
    t.year = 2024;
    t.comment = "warm-up";
    t.durationMs = 361234;
    t.sampleRate = 44100;
    t.bitrateKbps = 320;
    t.fileSize = 14123456;
    t.bpm = 122.0;
    t.bpmLocked = true;
    t.musicalKey = 21;
    t.keyConfidence = 0.83;
    t.loudnessLufs = -8.5;
    t.autoGainDb = -5.5;
    t.rating = 4;
    t.color = 0xE03030;
    t.playCount = 7;
    t.lastPlayed = 1763500000;
    t.dateAdded = 1763400000;
    t.analysisVersion = 1;
    t.missing = false;
    return t;
}
} // namespace

// --- tracks -----------------------------------------------------------------------------------------------------

TEST_CASE("TrackRepository: create, read, update, delete", "[library][repo]")
{
    Lib lib("tracks");
    auto original = fullTrack();
    std::int64_t id = 0;
    lib.write(
        [&](Connection& c)
        {
            auto r = TrackRepository(c).add(original);
            CHECK(r.isOk());
            id = r ? r.value() : 0;
        });
    REQUIRE(id > 0);
    CHECK(lib.takeChanges() == std::vector<Change>{{Table::Tracks, id, false}});

    TrackRepository tracks(*lib.reader);
    auto read = tracks.get(id);
    REQUIRE(read.has_value());
    original.id = id;
    CHECK(*read == original); // every column round-trips
    CHECK(tracks.findByPath(original.path) == read);
    CHECK_FALSE(tracks.findByPath("C:\\nope.mp3").has_value());
    CHECK_FALSE(tracks.get(999).has_value());

    // Empty text and missing numbers are NULL, and come back empty.
    std::int64_t bare = 0;
    lib.write(
        [&](Connection& c)
        {
            Track t;
            t.path = "C:\\b.mp3";
            t.contentHash = "b";
            bare = TrackRepository(c).add(t).value();
        });
    const auto b = tracks.get(bare);
    REQUIRE(b.has_value());
    CHECK(b->title.empty());
    CHECK_FALSE(b->bpm.has_value());
    CHECK_FALSE(b->color.has_value());
    CHECK(b->dateAdded > 1700000000); // 0 = now
    CHECK(lib.reader->queryInt("SELECT count(*) FROM tracks WHERE title IS NULL") == 1);

    CHECK(tracks.count() == 2);
    CHECK(tracks.list().size() == 2);
    CHECK(tracks.list(1, 10).size() == 1);
    CHECK(tracks.list(0, 1).front().id == id);
    lib.takeChanges();

    // Update.
    auto changed = *read;
    changed.title = "Night Drive (Extended)";
    changed.rating = 5;
    changed.color.reset();
    changed.bpm = 122.5;
    lib.write([&](Connection& c) { CHECK(TrackRepository(c).update(changed)); });
    CHECK(tracks.get(id) == changed);
    CHECK(lib.takeChanges() == std::vector<Change>{{Table::Tracks, id, false}});

    // Bad values are refused with a reason, and nothing changes.
    lib.write(
        [&](Connection& c)
        {
            TrackRepository repo(c);
            auto bad = changed;
            bad.rating = 6;
            CHECK_FALSE(repo.update(bad));
            CHECK(repo.error().find("rating") != std::string::npos);
            bad = changed;
            bad.musicalKey = 24;
            CHECK_FALSE(repo.update(bad));
            bad = changed;
            bad.path.clear();
            CHECK_FALSE(repo.add(bad).isOk());
            bad = changed;
            bad.id = 999;
            CHECK_FALSE(repo.update(bad));
            CHECK_FALSE(repo.add(changed).isOk()); // same path twice
            CHECK(repo.error().find("UNIQUE") != std::string::npos);
        });
    CHECK(tracks.get(id) == changed);
    CHECK(lib.takeChanges().empty());

    // Delete.
    lib.write([&](Connection& c) { CHECK(TrackRepository(c).remove(id)); });
    CHECK_FALSE(tracks.get(id).has_value());
    CHECK(tracks.count() == 1);
    CHECK(has(lib.takeChanges(), {Table::Tracks, id, true}));
    lib.write([&](Connection& c) { CHECK_FALSE(TrackRepository(c).remove(id)); });
}

TEST_CASE("TrackRepository: deleting a track takes its cues, tags and playlist entries, keeps history", "[library][repo]")
{
    Lib lib("track-delete");
    const auto a = lib.addTrack("C:\\a.mp3");
    const auto b = lib.addTrack("C:\\b.mp3");
    std::int64_t playlist = 0;
    lib.write(
        [&](Connection& c)
        {
            PlaylistRepository playlists(c);
            playlist = playlists.create({0, std::nullopt, "Set", PlaylistKind::Playlist}).value();
            const std::int64_t ids[] = {b, a, b};
            CHECK(playlists.setTracks(playlist, ids));
            CHECK(CueRepository(c).add({0, a, CueKind::HotCue, 0, 1000}).isOk());
            TagRepository tags(c);
            const auto tag = tags.createTag("Peak", std::nullopt).value();
            CHECK(tags.assign(a, tag));
            CHECK(c.exec("INSERT INTO history (session_id, track_id, played_at, previous_track_id) VALUES "
                         "(1, " + std::to_string(b) + ", 10, NULL), (1, " + std::to_string(a) + ", 20, " +
                         std::to_string(b) + "), (1, " + std::to_string(b) + ", 30, " + std::to_string(a) + ")"));
        });
    lib.takeChanges();

    lib.write([&](Connection& c) { CHECK(TrackRepository(c).remove(a)); });
    const auto changes = lib.takeChanges();
    CHECK(has(changes, {Table::Tracks, a, true}));
    CHECK(has(changes, {Table::Cues, a, false}));
    CHECK(has(changes, {Table::TrackTags, a, false}));
    CHECK(has(changes, {Table::PlaylistTracks, playlist, false}));

    CHECK(CueRepository(*lib.reader).forTrack(a).empty());
    CHECK(TagRepository(*lib.reader).tagsForTrack(a).empty());
    CHECK(PlaylistRepository(*lib.reader).trackIds(playlist) == std::vector<std::int64_t>{b, b});
    CHECK(lib.reader->queryInt("SELECT count(*) FROM history") == 3);
    CHECK(lib.reader->queryInt("SELECT count(*) FROM history WHERE track_id IS NULL") == 1);
    CHECK(lib.reader->queryInt("SELECT count(*) FROM history WHERE previous_track_id IS NULL") == 2);
}

// --- playlists --------------------------------------------------------------------------------------------------

TEST_CASE("PlaylistRepository: folders and playlists", "[library][repo]")
{
    Lib lib("playlists");
    std::int64_t folder = 0, sub = 0, p1 = 0, p2 = 0, top = 0;
    lib.write(
        [&](Connection& c)
        {
            PlaylistRepository repo(c);
            folder = repo.create({0, std::nullopt, "  Gigs  ", PlaylistKind::Folder}).value();
            sub = repo.create({0, folder, "2026", PlaylistKind::Folder}).value();
            p1 = repo.create({0, sub, "Club Night", PlaylistKind::Playlist}).value();
            p2 = repo.create({0, sub, "After Party", PlaylistKind::Playlist}).value();
            top = repo.create({0, std::nullopt, "Favourites", PlaylistKind::Playlist}).value();

            CHECK_FALSE(repo.create({0, p1, "Inside a playlist", PlaylistKind::Playlist}).isOk());
            CHECK(repo.error().find("folder") != std::string::npos);
            CHECK_FALSE(repo.create({0, std::nullopt, "   ", PlaylistKind::Playlist}).isOk());
            CHECK_FALSE(repo.create({0, 999, "Nowhere", PlaylistKind::Playlist}).isOk());
        });
    auto changes = lib.takeChanges();
    CHECK(changes.size() == 5);
    CHECK(has(changes, {Table::Playlists, p2, false}));

    PlaylistRepository repo(*lib.reader);
    const auto f = repo.get(folder);
    REQUIRE(f.has_value());
    CHECK(f->name == "Gigs"); // trimmed
    CHECK(f->kind == PlaylistKind::Folder);
    CHECK(f->createdAt > 1700000000);

    // Children come in sort order (creation order by default), the top level is parentId nullopt.
    const auto top_level = repo.children(std::nullopt);
    REQUIRE(top_level.size() == 2);
    CHECK(top_level[0].id == folder);
    CHECK(top_level[1].id == top);
    const auto inSub = repo.children(sub);
    REQUIRE(inSub.size() == 2);
    CHECK(inSub[0].name == "Club Night");
    CHECK(inSub[1].name == "After Party");
    CHECK(repo.all().size() == 5);

    // Update: rename, reorder, move; a folder can't go inside itself; the kind can't change.
    lib.write(
        [&](Connection& c)
        {
            PlaylistRepository w(c);
            auto p = *w.get(p2);
            p.name = "Afters";
            p.sortOrder = -1;
            CHECK(w.update(p));
            auto moved = *w.get(top);
            moved.parentId = folder;
            CHECK(w.update(moved));

            auto loop = *w.get(folder);
            loop.parentId = sub; // sub is inside folder
            CHECK_FALSE(w.update(loop));
            CHECK(w.error().find("itself") != std::string::npos);
            loop.parentId = folder;
            CHECK_FALSE(w.update(loop));
            auto kind = *w.get(p1);
            kind.kind = PlaylistKind::Folder;
            CHECK_FALSE(w.update(kind));
            auto intoPlaylist = *w.get(p1);
            intoPlaylist.parentId = p2;
            CHECK_FALSE(w.update(intoPlaylist));
        });
    CHECK(repo.children(sub).front().name == "Afters");
    CHECK(repo.get(top)->parentId == folder);
    CHECK(lib.takeChanges().size() == 2);

    // Delete a folder: everything inside goes with it.
    lib.write([&](Connection& c) { CHECK(PlaylistRepository(c).remove(folder)); });
    CHECK(repo.all().empty());
    changes = lib.takeChanges();
    for (const auto id : {folder, sub, p1, p2, top})
        CHECK(has(changes, {Table::Playlists, id, true}));
    lib.write([&](Connection& c) { CHECK_FALSE(PlaylistRepository(c).remove(folder)); });
}

TEST_CASE("PlaylistRepository: track lists", "[library][repo]")
{
    Lib lib("playlist-tracks");
    std::vector<std::int64_t> t;
    for (int i = 0; i < 5; ++i)
        t.push_back(lib.addTrack("C:\\" + std::to_string(i) + ".mp3"));
    std::int64_t p = 0, folder = 0, smart = 0;
    lib.write(
        [&](Connection& c)
        {
            PlaylistRepository repo(c);
            p = repo.create({0, std::nullopt, "Set", PlaylistKind::Playlist}).value();
            folder = repo.create({0, std::nullopt, "Folder", PlaylistKind::Folder}).value();
            smart = repo.create({0, std::nullopt, "Smart", PlaylistKind::Smart, R"({"bpm":[120,130]})"}).value();
        });
    lib.takeChanges();
    PlaylistRepository read(*lib.reader);
    CHECK(read.get(smart)->ruleJson == R"({"bpm":[120,130]})");

    lib.write(
        [&](Connection& c)
        {
            PlaylistRepository repo(c);
            const std::int64_t first[] = {t[0], t[1], t[2]};
            CHECK(repo.addTracks(p, first));
            const std::int64_t more[] = {t[3], t[0]}; // a track may be in a playlist twice
            CHECK(repo.addTracks(p, more, 1));
            CHECK(repo.trackIds(p) == std::vector<std::int64_t>{t[0], t[3], t[0], t[1], t[2]});

            CHECK(repo.moveTrack(p, 4, 0));
            CHECK(repo.trackIds(p) == std::vector<std::int64_t>{t[2], t[0], t[3], t[0], t[1]});
            const int drop[] = {1, 3};
            CHECK(repo.removeTracksAt(p, drop));
            CHECK(repo.trackIds(p) == std::vector<std::int64_t>{t[2], t[3], t[1]});
            CHECK_FALSE(repo.moveTrack(p, 0, 3));
            const int outOfRange[] = {7};
            CHECK_FALSE(repo.removeTracksAt(p, outOfRange));

            const std::int64_t one[] = {t[4]};
            CHECK_FALSE(repo.addTracks(folder, one));
            CHECK(repo.error().find("folders") != std::string::npos);
            CHECK_FALSE(repo.addTracks(smart, one));
            const std::int64_t missing[] = {999};
            CHECK_FALSE(repo.addTracks(p, missing)); // FOREIGN KEY: no such track
            CHECK(repo.trackIds(p) == std::vector<std::int64_t>{t[2], t[3], t[1]}); // unchanged
        });
    CHECK(read.trackIds(p) == std::vector<std::int64_t>{t[2], t[3], t[1]});
    const auto changes = lib.takeChanges();
    CHECK(changes.size() == 4); // two adds, a move, a removal
    for (const auto& c : changes)
        CHECK(c == Change{Table::PlaylistTracks, p, false});

    lib.write([&](Connection& c) { CHECK(PlaylistRepository(c).setTracks(p, {})); });
    CHECK(read.trackIds(p).empty());
}

// --- cues -------------------------------------------------------------------------------------------------------

TEST_CASE("CueRepository: create, read, update, delete; one cue per pad", "[library][repo]")
{
    Lib lib("cues");
    const auto track = lib.addTrack("C:\\a.mp3");
    const auto other = lib.addTrack("C:\\b.mp3");
    std::int64_t hot0 = 0, memory = 0, loop = 0;
    lib.write(
        [&](Connection& c)
        {
            CueRepository repo(c);
            hot0 = repo.add({0, track, CueKind::HotCue, 0, 48000, 0, "Drop", 0x30E030}).value();
            memory = repo.add({0, track, CueKind::MemoryCue, std::nullopt, 96000}).value();
            loop = repo.add({0, track, CueKind::MemoryLoop, std::nullopt, 192000, 96000, "Outro loop"}).value();
            CHECK(repo.add({0, other, CueKind::HotCue, 0, 10}).isOk());

            CHECK_FALSE(repo.add({0, track, CueKind::HotCue, std::nullopt, 0}).isOk()); // needs a pad
            CHECK_FALSE(repo.add({0, track, CueKind::HotCue, 16, 0}).isOk());
            CHECK_FALSE(repo.add({0, track, CueKind::MemoryCue, 3, 0}).isOk()); // no pad for memory cues
            CHECK_FALSE(repo.add({0, track, CueKind::MemoryLoop, std::nullopt, 0, 0}).isOk()); // needs a length
            CHECK_FALSE(repo.add({0, track, CueKind::MemoryCue, std::nullopt, -5}).isOk());
            CHECK_FALSE(repo.add({0, 999, CueKind::MemoryCue, std::nullopt, 0}).isOk()); // no such track
        });
    auto changes = lib.takeChanges();
    CHECK(has(changes, {Table::Cues, track, false}));
    CHECK(has(changes, {Table::Cues, other, false}));

    CueRepository read(*lib.reader);
    const auto hot = read.get(hot0);
    REQUIRE(hot.has_value());
    CHECK(*hot == Cue{hot0, track, CueKind::HotCue, 0, 48000, 0, "Drop", 0x30E030});
    CHECK(read.hotCue(track, 0) == hot);
    CHECK_FALSE(read.hotCue(track, 1).has_value());
    auto all = read.forTrack(track);
    REQUIRE(all.size() == 3);
    CHECK(all[0].id == hot0); // hot cues first, then by position
    CHECK(all[1].id == memory);
    CHECK(all[2].id == loop);

    // A new hot cue (or hot loop) on pad 0 replaces the old one.
    std::int64_t hotLoop = 0;
    lib.write(
        [&](Connection& c)
        {
            CueRepository repo(c);
            hotLoop = repo.add({0, track, CueKind::HotLoop, 0, 1000, 4000}).value();
        });
    CHECK_FALSE(read.get(hot0).has_value());
    CHECK(read.hotCue(track, 0)->id == hotLoop);
    CHECK(read.hotCue(other, 0).has_value()); // other tracks' pads are untouched

    // Update: move the hot loop to pad 2, relabel the memory cue; a cue can't change track.
    lib.write(
        [&](Connection& c)
        {
            CueRepository repo(c);
            CHECK(repo.add({0, track, CueKind::HotCue, 2, 5}).isOk());
            auto moved = *repo.get(hotLoop);
            moved.slot = 2;
            CHECK(repo.update(moved)); // replaces the cue on pad 2
            auto m = *repo.get(memory);
            m.label = "Breakdown";
            m.color = 0x2040FF;
            CHECK(repo.update(m));
            m.trackId = other;
            CHECK_FALSE(repo.update(m));
            auto badLoop = *repo.get(loop);
            badLoop.lengthSamples = 0;
            CHECK_FALSE(repo.update(badLoop));
        });
    CHECK(read.hotCue(track, 2)->id == hotLoop);
    CHECK_FALSE(read.hotCue(track, 0).has_value());
    CHECK(read.get(memory)->label == "Breakdown");
    CHECK(read.get(loop)->lengthSamples == 96000);
    CHECK(read.forTrack(track).size() == 3);

    // Delete.
    lib.takeChanges();
    lib.write([&](Connection& c) { CHECK(CueRepository(c).remove(memory)); });
    CHECK_FALSE(read.get(memory).has_value());
    CHECK(lib.takeChanges() == std::vector<Change>{{Table::Cues, track, false}});
    lib.write([&](Connection& c) { CHECK(CueRepository(c).removeAllForTrack(track)); });
    CHECK(read.forTrack(track).empty());
    CHECK(read.forTrack(other).size() == 1);
    lib.write([&](Connection& c) { CHECK_FALSE(CueRepository(c).remove(memory)); });
}

// --- tags -------------------------------------------------------------------------------------------------------

TEST_CASE("TagRepository: categories, tags and track assignments", "[library][repo]")
{
    Lib lib("tags");
    const auto a = lib.addTrack("C:\\a.mp3");
    const auto b = lib.addTrack("C:\\b.mp3");
    std::int64_t mood = 0, situation = 0, dark = 0, warm = 0, peak = 0, loose = 0;
    lib.write(
        [&](Connection& c)
        {
            TagRepository repo(c);
            mood = repo.createCategory("Mood").value();
            situation = repo.createCategory("Situation").value();
            CHECK_FALSE(repo.createCategory(" mood ").isOk()); // names are unique, ignoring case
            CHECK_FALSE(repo.createCategory("").isOk());

            dark = repo.createTag("Dark", mood).value();
            warm = repo.createTag("Warm-up", situation).value();
            peak = repo.createTag("Peak time", situation).value();
            loose = repo.createTag("Vinyl rip", std::nullopt).value();
            CHECK(repo.createTag("Dark", situation).isOk()); // same name, other category: fine
            CHECK_FALSE(repo.createTag("DARK", mood).isOk());
            CHECK_FALSE(repo.createTag("x", 999).isOk());

            CHECK(repo.assign(a, dark));
            CHECK(repo.assign(a, peak));
            CHECK(repo.assign(a, peak)); // twice is fine
            CHECK(repo.assign(b, peak));
            CHECK_FALSE(repo.assign(999, peak));
            CHECK_FALSE(repo.assign(a, 999));
        });
    auto changes = lib.takeChanges();
    CHECK(has(changes, {Table::TagCategories, mood, false}));
    CHECK(has(changes, {Table::Tags, warm, false}));
    CHECK(has(changes, {Table::TrackTags, a, false}));
    CHECK(has(changes, {Table::TrackTags, b, false}));

    TagRepository read(*lib.reader);
    const auto cats = read.categories();
    REQUIRE(cats.size() == 2);
    CHECK(cats[0].name == "Mood");
    CHECK(cats[1].name == "Situation");
    CHECK(read.tag(dark) == Tag{dark, mood, "Dark"});
    CHECK(read.tags().size() == 5);
    const auto inSituation = read.tagsInCategory(situation);
    REQUIRE(inSituation.size() == 3);
    CHECK(inSituation[0].name == "Dark");
    CHECK(inSituation[1].name == "Peak time");
    CHECK(read.tagsInCategory(std::nullopt) == std::vector<Tag>{{loose, std::nullopt, "Vinyl rip"}});
    const auto onA = read.tagsForTrack(a);
    REQUIRE(onA.size() == 2);
    CHECK(onA[0].id == dark);
    CHECK(onA[1].id == peak);
    CHECK(read.tracksWithTag(peak) == std::vector<std::int64_t>{a, b});

    // Update: rename and reorder a category, rename and move a tag.
    lib.write(
        [&](Connection& c)
        {
            TagRepository repo(c);
            CHECK(repo.updateCategory({situation, "When", -1}));
            CHECK_FALSE(repo.updateCategory({mood, "when", 0})); // taken
            CHECK(repo.updateTag({warm, mood, "Warm"}));
            CHECK_FALSE(repo.updateTag({peak, mood, "warm"}));
            CHECK_FALSE(repo.updateTag({999, mood, "x"}));
        });
    CHECK(read.categories().front().name == "When");
    CHECK(read.tag(warm) == Tag{warm, mood, "Warm"});

    // Unassign, delete a tag (assignments go), delete a category (its tags go).
    lib.takeChanges();
    lib.write(
        [&](Connection& c)
        {
            TagRepository repo(c);
            CHECK(repo.unassign(a, dark));
            CHECK(repo.removeTag(peak));
        });
    CHECK(read.tagsForTrack(a).empty());
    CHECK(read.tagsForTrack(b).empty());
    changes = lib.takeChanges();
    CHECK(has(changes, {Table::Tags, peak, true}));
    CHECK(has(changes, {Table::TrackTags, b, false}));

    lib.write([&](Connection& c) { CHECK(TagRepository(c).removeCategory(mood)); });
    CHECK_FALSE(read.category(mood).has_value());
    CHECK_FALSE(read.tag(dark).has_value());
    CHECK_FALSE(read.tag(warm).has_value());
    CHECK(read.tag(loose).has_value());
    changes = lib.takeChanges();
    CHECK(has(changes, {Table::TagCategories, mood, true}));
    CHECK(has(changes, {Table::Tags, dark, true}));
}

// --- change notifications ---------------------------------------------------------------------------------------

TEST_CASE("Change notifications: per write job, after it ran; rolled-back work isn't reported", "[library][repo]")
{
    Lib lib("notify");

    // One job = one notification with everything it changed.
    std::atomic<int> calls{0};
    const int counter = lib.db->addChangeListener([&](const std::vector<Change>&) { ++calls; });
    std::int64_t a = 0, b = 0;
    lib.write(
        [&](Connection& c)
        {
            TrackRepository repo(c);
            Track t;
            t.contentHash = "h";
            t.path = "C:\\a.mp3";
            a = repo.add(t).value();
            t.path = "C:\\b.mp3";
            b = repo.add(t).value();
        });
    CHECK(calls == 1); // already delivered when writeAndWait returns
    CHECK(lib.takeChanges() == std::vector<Change>{{Table::Tracks, a, false}, {Table::Tracks, b, false}});

    // A transaction that rolls back reports nothing, and its rows are gone.
    lib.write(
        [&](Connection& c)
        {
            const bool ok = c.transaction(
                [&]
                {
                    TrackRepository repo(c);
                    Track t;
                    t.contentHash = "h";
                    t.path = "C:\\c.mp3";
                    CHECK(repo.add(t).isOk());
                    return repo.add(t).isOk(); // same path: fails, the whole transaction rolls back
                });
            CHECK_FALSE(ok);
        });
    CHECK(lib.takeChanges().empty());
    CHECK(TrackRepository(*lib.reader).count() == 2);

    // Async writes notify too; a removed listener hears nothing more.
    lib.db->removeChangeListener(counter);
    lib.db->write([&](Connection& c) { TrackRepository(c).remove(a); });
    lib.db->flush();
    CHECK(calls == 1);
    CHECK(has(lib.takeChanges(), {Table::Tracks, a, true})); // plus its cues and tags

    // A job that only reads changes nothing and notifies nobody.
    lib.write([&](Connection& c) { TrackRepository(c).count(); });
    CHECK(lib.takeChanges().empty());
}

TEST_CASE("Repositories refuse to run on the audio thread", "[library][repo]")
{
    if (!devildj::util::kRealtimeGuardEnabled)
        SKIP("Debug builds only (RealtimeGuard)");
    Lib lib("audio-thread");
    const auto id = lib.addTrack("C:\\a.mp3");
    static std::atomic<int> violations{0};
    const auto previous = devildj::util::setRealtimeViolationHandler([](devildj::util::RealtimeViolation) noexcept
                                                                     { ++violations; });
    std::optional<Track> found;
    std::int64_t count = -1;
    {
        devildj::util::ScopedRealtimeContext audio;
        TrackRepository repo(*lib.reader);
        found = repo.get(id);
        count = repo.count();
    }
    devildj::util::setRealtimeViolationHandler(previous);
    CHECK_FALSE(found.has_value());
    CHECK(count == 0);
    CHECK(TrackRepository(*lib.reader).get(id).has_value()); // fine off the audio thread
}
