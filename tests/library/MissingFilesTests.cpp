// P2-10: missing files. Done when: moving a folder and relocating restores all links.

#include "TestData.h"
#include "library/Connection.h"
#include "library/CueRepository.h"
#include "library/Database.h"
#include "library/Importer.h"
#include "library/MissingFiles.h"
#include "library/PlaylistRepository.h"
#include "library/TagRepository.h"
#include "library/TrackRepository.h"
#include "ui/LibraryBrowser.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace devildj::library;
namespace fs = std::filesystem;

namespace
{
std::string readBytes(const fs::path& f)
{
    std::ifstream in(f, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void writeBytes(const fs::path& f, const std::string& bytes)
{
    fs::create_directories(f.parent_path());
    std::ofstream out(f, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

fs::path generated(const char* relative)
{
    return fs::path(devildj::test::generatedFile(relative).getFullPathName().toWideCharPointer());
}

// A playable MP3 whose content is unique (an ID3v1 tag with `tag` in it).
void makeTrack(const fs::path& f, const std::string& tag)
{
    std::string id3(128, '\0');
    id3.replace(0, 3, "TAG");
    id3.replace(3, std::min<std::size_t>(30, tag.size()), tag.substr(0, 30));
    writeBytes(f, readBytes(generated("formats/ref.mp3")) + id3);
}

struct Lib
{
    fs::path dir;
    std::unique_ptr<Database> db;
    std::unique_ptr<Connection> reader;
    Lib()
    {
        dir = fs::temp_directory_path() /
              ("devildj-missing-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        db = std::move(Database::open(dir / "library.db")).value();
        reader = std::move(db->openReader()).value();
    }
    ~Lib()
    {
        reader.reset();
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Track byFile(const fs::path& f) { return *TrackRepository(*reader).findByPath(libraryPath(f)); }
    Track get(std::int64_t id) { return *TrackRepository(*reader).get(id); }
    std::int64_t missingCount() { return reader->queryInt("SELECT count(*) FROM tracks WHERE missing = 1").value_or(-1); }
};
} // namespace

TEST_CASE("Missing files: moving a folder and relocating restores all links", "[library][missing]")
{
    if (!fs::exists(generated("formats/ref.mp3")))
        SKIP("Generated test signals missing (python tools/gen_test_signals.py)");
    Lib lib;
    const auto oldMusic = lib.dir / "Music";
    const std::vector<fs::path> rel{"a.mp3", "Sub/b.mp3", fs::path(u8"폴더/c.mp3"), "Sub/Deeper/d.mp3"};
    for (std::size_t i = 0; i < rel.size(); ++i)
        makeTrack(oldMusic / rel[i], "t" + std::to_string(i));
    const std::vector<fs::path> inputs{oldMusic};
    REQUIRE(importTracks(*lib.db, inputs).added == 4);

    // Links to the tracks: a playlist, cues, a tag, a rating.
    std::vector<std::int64_t> ids;
    for (const auto& r : rel)
        ids.push_back(lib.byFile(oldMusic / r).id);
    std::int64_t playlist = 0, tag = 0;
    lib.db->writeAndWait(
        [&](Connection& c)
        {
            PlaylistRepository playlists(c);
            playlist = playlists.create({0, std::nullopt, "Set", PlaylistKind::Playlist}).value();
            const std::int64_t order[] = {ids[3], ids[0], ids[2], ids[1]};
            CHECK(playlists.setTracks(playlist, order));
            CueRepository cues(c);
            CHECK(cues.add({0, ids[1], CueKind::HotCue, 0, 4410}).isOk());
            CHECK(cues.add({0, ids[2], CueKind::MemoryLoop, std::nullopt, 100, 2000}).isOk());
            TagRepository tags(c);
            tag = tags.createTag("Peak", std::nullopt).value();
            CHECK(tags.assign(ids[3], tag));
            CHECK(TrackRepository(c).setRating(ids[0], 5));
        });

    // Move the whole folder somewhere else.
    const auto newMusic = lib.dir / "Moved" / "Music library";
    fs::create_directories(newMusic.parent_path());
    fs::rename(oldMusic, newMusic);

    // The check finds all four missing.
    const auto scan = scanForMissingFiles(*lib.db);
    CHECK(scan.checked == 4);
    CHECK(scan.missing == 4);
    CHECK(scan.newlyMissing == 4);
    CHECK(lib.missingCount() == 4);

    // Relocate: search the new place.
    const auto r = relocateMissingFiles(*lib.db, newMusic.parent_path());
    CHECK(r.lookedFor == 4);
    CHECK(r.relocated == 4);
    CHECK(r.notFound == 0);
    CHECK(lib.missingCount() == 0);

    // Same ids, new paths, every link intact.
    for (std::size_t i = 0; i < rel.size(); ++i)
    {
        const auto t = lib.get(ids[i]);
        CHECK(t.path == libraryPath(newMusic / rel[i]));
        CHECK_FALSE(t.missing);
    }
    CHECK(PlaylistRepository(*lib.reader).trackIds(playlist) == std::vector<std::int64_t>{ids[3], ids[0], ids[2], ids[1]});
    CHECK(CueRepository(*lib.reader).forTrack(ids[1]).size() == 1);
    CHECK(CueRepository(*lib.reader).forTrack(ids[2]).at(0).lengthSamples == 2000);
    CHECK(TagRepository(*lib.reader).tracksWithTag(tag) == std::vector<std::int64_t>{ids[3]});
    CHECK(lib.get(ids[0]).rating == 5);

    // Checking again: nothing missing; nothing changes.
    const auto again = scanForMissingFiles(*lib.db);
    CHECK(again.missing == 0);
    CHECK(again.newlyMissing == 0);
    CHECK(lib.reader->queryInt("SELECT count(*) FROM tracks") == 4);
}

TEST_CASE("Relocate: name + size, the content decides between look-alikes, others left alone", "[library][missing]")
{
    if (!fs::exists(generated("formats/ref.mp3")))
        SKIP("Generated test signals missing (python tools/gen_test_signals.py)");
    Lib lib;
    makeTrack(lib.dir / "Old" / "Intro.mp3", "intro");
    makeTrack(lib.dir / "Old" / "Outro.mp3", "outro");
    makeTrack(lib.dir / "Old" / "Gone.mp3", "gone");
    const std::vector<fs::path> inputs{lib.dir / "Old"};
    REQUIRE(importTracks(*lib.db, inputs).added == 3);
    const auto intro = lib.byFile(lib.dir / "Old" / "Intro.mp3").id;
    const auto outro = lib.byFile(lib.dir / "Old" / "Outro.mp3").id;
    const auto gone = lib.byFile(lib.dir / "Old" / "Gone.mp3").id;

    // Intro moved and renamed in case; two files called Outro.mp3 of the same size, only one is ours;
    // Gone.mp3 exists elsewhere but with a different size (another song): not ours.
    fs::create_directories(lib.dir / "New" / "A");
    fs::create_directories(lib.dir / "New" / "B");
    fs::rename(lib.dir / "Old" / "Intro.mp3", lib.dir / "New" / "INTRO.MP3");
    makeTrack(lib.dir / "New" / "A" / "Outro.mp3", "other"); // same size, other content
    fs::rename(lib.dir / "Old" / "Outro.mp3", lib.dir / "New" / "B" / "Outro.mp3");
    fs::remove(lib.dir / "Old" / "Gone.mp3");
    writeBytes(lib.dir / "New" / "Gone.mp3", "a different, smaller file");

    // Only some tracks: Outro alone first.
    const std::int64_t onlyOutro[] = {outro};
    auto r = relocateMissingFiles(*lib.db, lib.dir / "New", onlyOutro);
    CHECK(r.lookedFor == 1);
    CHECK(r.relocated == 1);
    CHECK(lib.get(outro).path == libraryPath(lib.dir / "New" / "B" / "Outro.mp3"));

    // Then the rest.
    r = relocateMissingFiles(*lib.db, lib.dir / "New");
    CHECK(r.lookedFor == 2);
    CHECK(r.relocated == 1);
    CHECK(r.notFound == 1);
    CHECK(lib.get(intro).path == libraryPath(lib.dir / "New" / "INTRO.MP3"));
    CHECK(lib.get(gone).path == libraryPath(lib.dir / "Old" / "Gone.mp3")); // still where it was, still missing
    scanForMissingFiles(*lib.db);
    CHECK(lib.get(gone).missing);
    CHECK(lib.missingCount() == 1);

    // A file that is already another library track isn't taken.
    makeTrack(lib.dir / "Elsewhere" / "Gone.mp3", "gone");
    const std::vector<fs::path> more{lib.dir / "Elsewhere" / "Gone.mp3"};
    // Importing it: same content as the missing track -> the track is relocated, not duplicated.
    const auto imported = importTracks(*lib.db, more);
    CHECK(imported.added == 0);
    CHECK(imported.relocated == 1);
    CHECK(imported.duplicates == 0);
    CHECK(lib.get(gone).path == libraryPath(lib.dir / "Elsewhere" / "Gone.mp3"));
    CHECK_FALSE(lib.get(gone).missing);
    // A real copy of a track whose file is there is still a duplicate.
    fs::copy_file(lib.dir / "Elsewhere" / "Gone.mp3", lib.dir / "Elsewhere" / "Gone copy.mp3");
    const std::vector<fs::path> copy{lib.dir / "Elsewhere" / "Gone copy.mp3"};
    const auto dup = importTracks(*lib.db, copy);
    CHECK(dup.duplicates == 1);
    CHECK(dup.relocated == 0);

    // A file that came back by itself is no longer missing after a check.
    setTrackMissing(*lib.db, intro, true);
    lib.db->flush();
    CHECK(lib.get(intro).missing);
    const auto back = scanForMissingFiles(*lib.db);
    CHECK(back.foundAgain == 1);
    CHECK_FALSE(lib.get(intro).missing);
}

TEST_CASE("Browser: Missing files lists them, in red, with their menu", "[ui][missing]")
{
    if (!fs::exists(generated("formats/ref.mp3")))
        SKIP("Generated test signals missing (python tools/gen_test_signals.py)");
    juce::ScopedJuceInitialiser_GUI gui;
    Lib lib;
    makeTrack(lib.dir / "M" / "here.mp3", "here");
    makeTrack(lib.dir / "M" / "gone.mp3", "gone");
    const std::vector<fs::path> inputs{lib.dir / "M"};
    REQUIRE(importTracks(*lib.db, inputs).added == 2);
    fs::remove(lib.dir / "M" / "gone.mp3");

    devildj::ui::LibraryBrowser browser(lib.db.get(), juce::File());
    browser.setBounds(0, 0, 1200, 400);
    CHECK(browser.treeRowNames()[1] == "Missing files");
    std::vector<std::int64_t> relocateAsked{-1};
    bool checkAsked = false;
    browser.onRelocate = [&](const std::vector<std::int64_t>& ids) { relocateAsked = ids; };
    browser.onCheckMissing = [&] { checkAsked = true; };

    browser.showMissing();
    CHECK(browser.model()->size() == 0); // not checked yet
    scanForMissingFiles(*lib.db);
    lib.db->flush();
    browser.handlePendingLibraryChanges();
    REQUIRE(browser.model()->size() == 1);
    CHECK(browser.model()->row(0)->missing);
    CHECK(browser.model()->row(0)->path.endsWith("gone.mp3"));

    // Loading a missing file says so; the menus offer to look for it.
    browser.moveSelection(1);
    CHECK(browser.loadSelected(0));
    CHECK(browser.status().contains("File not found"));
    const auto menu = browser.menuForMissing();
    CHECK(menu.getNumItems() == 2);
    for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
        if (it.getItem().action)
            it.getItem().action();
    CHECK(checkAsked);
    CHECK(relocateAsked.empty()); // every missing track
    bool hasRelocate = false;
    const auto trackMenu = browser.menuForTracks(); // keep it alive while iterating
    for (juce::PopupMenu::MenuItemIterator it(trackMenu); it.next();)
        hasRelocate |= it.getItem().text.startsWith("Relocate");
    CHECK(hasRelocate);

    // In the collection too, the missing one is marked.
    browser.showCollection();
    int marked = 0;
    for (int i = 0; i < browser.model()->size(); ++i)
        marked += browser.model()->row(i)->missing ? 1 : 0;
    CHECK(marked == 1);
}
