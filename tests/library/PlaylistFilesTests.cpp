// P2-06: M3U / M3U8 playlists. Done when: round-trip M3U8 test.

#include "TestData.h"
#include "library/Connection.h"
#include "library/Database.h"
#include "library/Importer.h"
#include "library/PlaylistFiles.h"
#include "library/PlaylistRepository.h"
#include "library/TrackRepository.h"

#include <catch2/catch_test_macros.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace devildj::library;
namespace fs = std::filesystem;

namespace
{
std::string utf8(const char8_t* t)
{
    return reinterpret_cast<const char*>(t);
}

std::string toUtf8(const fs::path& p)
{
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

struct Folder
{
    fs::path dir;
    explicit Folder(const char* name)
    {
        dir = fs::temp_directory_path() / (std::string("devildj-m3u-") + name + "-" +
                                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
    }
    ~Folder()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

void writeBytes(const fs::path& f, const std::string& bytes)
{
    fs::create_directories(f.parent_path());
    std::ofstream out(f, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string readBytes(const fs::path& f)
{
    std::ifstream in(f, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

fs::path generated(const char* relative)
{
    return fs::path(devildj::test::generatedFile(relative).getFullPathName().toWideCharPointer());
}

// A playable copy of the test MP3 with a unique ID3v1 tag (so each has its own content hash).
void makeTrack(const fs::path& target, const std::string& tag)
{
    std::string bytes = readBytes(generated("formats/ref.mp3"));
    std::string id3(128, '\0');
    id3.replace(0, 3, "TAG");
    id3.replace(3, std::min<std::size_t>(30, tag.size()), tag.substr(0, 30));
    writeBytes(target, bytes + id3);
}

std::unique_ptr<Database> openDb(const fs::path& file)
{
    auto opened = Database::open(file);
    REQUIRE(opened.isOk());
    return std::move(opened).value();
}

std::vector<std::string> playlistPaths(Database& db, std::int64_t playlist)
{
    auto reader = std::move(db.openReader()).value();
    std::vector<std::string> out;
    auto* s = reader->prepare("SELECT t.path FROM playlist_tracks pt JOIN tracks t ON t.id = pt.track_id "
                              "WHERE pt.playlist_id = ? ORDER BY pt.position");
    s->bind(1, playlist);
    while (s->step())
        out.push_back(s->getText(0));
    return out;
}
} // namespace

TEST_CASE("M3U8: a library playlist round-trips through a file", "[library][m3u]")
{
    if (!fs::exists(generated("formats/ref.mp3")))
        SKIP("Generated test signals missing (python tools/gen_test_signals.py)");
    Folder tmp("roundtrip");
    const auto music = tmp.dir / "Music";
    const std::vector<fs::path> files{music / "01 Opener.mp3", music / fs::path(u8"한국어 폴더") / u8"두번째.mp3",
                                      music / "Sub" / "third, with comma.mp3"};
    for (std::size_t i = 0; i < files.size(); ++i)
        makeTrack(files[i], "t" + std::to_string(i));

    // Library A: import the files, make a playlist in a chosen order (one track twice).
    auto a = openDb(tmp.dir / "a.db");
    const std::vector<fs::path> inputs{music};
    REQUIRE(importTracks(*a, inputs).added == 3);
    std::int64_t playlist = 0;
    a->writeAndWait(
        [&](Connection& c)
        {
            TrackRepository tracks(c);
            std::vector<std::int64_t> ids;
            for (const auto& f : files)
            {
                auto t = tracks.findByPath(libraryPath(f));
                CHECK(t.has_value());
                if (!t)
                    return;
                ids.push_back(t->id);
            }
            auto second = *tracks.get(ids[1]);
            second.title = utf8(u8"두번째 트랙");
            second.artist = utf8(u8"김서연");
            CHECK(tracks.update(second));
            PlaylistRepository repo(c);
            playlist = repo.create({0, std::nullopt, "Friday", PlaylistKind::Playlist}).value();
            const std::int64_t order[] = {ids[2], ids[0], ids[1], ids[0]};
            CHECK(repo.setTracks(playlist, order));
        });
    REQUIRE(playlist > 0);
    const auto before = playlistPaths(*a, playlist);
    REQUIRE(before.size() == 4);

    // Export.
    const auto m3u = tmp.dir / "Friday.m3u8";
    {
        auto reader = std::move(a->openReader()).value();
        const auto written = exportPlaylist(*reader, playlist, m3u);
        REQUIRE(written.isOk());
        CHECK(written.value() == 4);
    }
    const auto text = readBytes(m3u);
    CHECK(text.rfind("#EXTM3U\r\n", 0) == 0); // no BOM, CRLF
    CHECK(text.find("\r\n" + libraryPath(files[0]) + "\r\n") != std::string::npos);
    CHECK(text.find(utf8(u8"#EXTINF:") ) != std::string::npos);
    CHECK(text.find(utf8(u8",김서연 - 두번째 트랙\r\n")) != std::string::npos);

    // Read back: same files, titles and order.
    const auto entries = readM3u(m3u);
    REQUIRE(entries.isOk());
    REQUIRE(entries.value().size() == 4);
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(libraryPath(entries.value()[i].file) == before[i]);
    CHECK(entries.value()[2].title == utf8(u8"김서연 - 두번째 트랙"));
    CHECK(entries.value()[0].seconds >= 0);

    // Import into a different, empty library: the tracks are added and the playlist is the same.
    auto b = openDb(tmp.dir / "b.db");
    const auto made = importPlaylist(*b, m3u);
    REQUIRE(made.isOk());
    CHECK(made.value().name == "Friday");
    CHECK(made.value().tracks == 4);
    CHECK(made.value().addedToLibrary == 3);
    CHECK(made.value().missing.empty());
    CHECK(playlistPaths(*b, made.value().playlistId) == before);

    // And back into library A: nothing new in the library, a second playlist with the same tracks.
    const auto again = importPlaylist(*a, m3u);
    REQUIRE(again.isOk());
    CHECK(again.value().addedToLibrary == 0);
    CHECK(playlistPaths(*a, again.value().playlistId) == before);

    // Export -> import -> export lists the same files (library B's titles come from the files' own tags).
    const auto m3u2 = tmp.dir / "again.m3u8";
    {
        auto reader = std::move(b->openReader()).value();
        REQUIRE(exportPlaylist(*reader, made.value().playlistId, m3u2).isOk());
    }
    const auto entries2 = readM3u(m3u2);
    REQUIRE(entries2.isOk());
    REQUIRE(entries2.value().size() == 4);
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(libraryPath(entries2.value()[i].file) == before[i]);
}

TEST_CASE("M3U: reading other software's files", "[library][m3u]")
{
    Folder tmp("read");
    const auto list = tmp.dir / "Lists" / "set.m3u";
    std::string body = "\xEF\xBB\xBF#EXTM3U\n"                     // BOM, LF line ends
                       "# a comment\n"
                       "\n"
                       "#EXTINF:301,Neon Owl - Midnight Drive\n"
                       "..\\Music\\a.mp3\n"                         // relative to the playlist
                       "b.flac\r\n"                                 // CRLF, no EXTINF
                       "\\Root\\c.wav\n"                            // root-relative: the playlist's drive
                       "file:///C:/My%20Music/d.mp3\n"              // a URL
                       "C:\\Abs\\e.mp3";                            // no line end at the end
    writeBytes(list, body);
    const auto r = readM3u(list);
    REQUIRE(r.isOk());
    const auto& e = r.value();
    REQUIRE(e.size() == 5);
    CHECK(e[0].file == (tmp.dir / "Music" / "a.mp3").lexically_normal());
    CHECK(e[0].title == "Neon Owl - Midnight Drive");
    CHECK(e[0].seconds == 301);
    CHECK(e[1].file == (tmp.dir / "Lists" / "b.flac").lexically_normal());
    CHECK(e[1].seconds == -1);
    CHECK(e[2].file == fs::path(tmp.dir.root_name() / "\\Root\\c.wav"));
    CHECK(e[3].file == fs::path("C:\\My Music\\d.mp3"));
    CHECK(e[4].file == fs::path("C:\\Abs\\e.mp3"));

    // An old .m3u written in the Windows code page (not UTF-8).
    if (GetACP() != CP_UTF8)
    {
        const wchar_t* wide = L"C:\\Music\\Caf\u00e9.mp3"; // é exists in most code pages
        char ansi[256] = {};
        BOOL lossy = FALSE;
        const int n = WideCharToMultiByte(CP_ACP, 0, wide, -1, ansi, sizeof(ansi), nullptr, &lossy);
        if (n > 0 && !lossy)
        {
            writeBytes(tmp.dir / "old.m3u", std::string(ansi) + "\r\n");
            const auto old = readM3u(tmp.dir / "old.m3u");
            REQUIRE(old.isOk());
            REQUIRE(old.value().size() == 1);
            CHECK(old.value()[0].file == fs::path(wide));
        }
    }

    CHECK_FALSE(readM3u(tmp.dir / "missing.m3u8").isOk());
    CHECK(isPlaylistFile("a.M3U8"));
    CHECK(isPlaylistFile("a.m3u"));
    CHECK_FALSE(isPlaylistFile("a.mp3"));
}

TEST_CASE("M3U import: missing files are reported, copies map to the library track", "[library][m3u]")
{
    if (!fs::exists(generated("formats/ref.mp3")))
        SKIP("Generated test signals missing (python tools/gen_test_signals.py)");
    Folder tmp("missing");
    const auto music = tmp.dir / "Music";
    makeTrack(music / "a.mp3", "a");
    makeTrack(music / "b.mp3", "b");
    auto db = openDb(tmp.dir / "lib.db");
    const std::vector<fs::path> inputs{music};
    REQUIRE(importTracks(*db, inputs).added == 2);

    // The playlist points at a copy of a.mp3 somewhere else, a file that's gone, and b.mp3.
    fs::create_directories(tmp.dir / "Backup");
    fs::copy_file(music / "a.mp3", tmp.dir / "Backup" / "a copy.mp3");
    writeM3u8(tmp.dir / "list.m3u8", {{tmp.dir / "Backup" / "a copy.mp3", "", -1},
                                      {music / "gone.mp3", "", -1},
                                      {music / "b.mp3", "", -1}});
    const auto r = importPlaylist(*db, tmp.dir / "list.m3u8");
    REQUIRE(r.isOk());
    CHECK(r.value().tracks == 2);
    CHECK(r.value().addedToLibrary == 0); // the copy is the same track
    REQUIRE(r.value().missing.size() == 1);
    CHECK(r.value().missing[0].find("gone.mp3") != std::string::npos);
    CHECK(playlistPaths(*db, r.value().playlistId) ==
          std::vector<std::string>{libraryPath(music / "a.mp3"), libraryPath(music / "b.mp3")});

    // Into a folder.
    std::int64_t folder = 0;
    db->writeAndWait([&](Connection& c)
                     { folder = PlaylistRepository(c).create({0, std::nullopt, "Gigs", PlaylistKind::Folder}).value(); });
    const auto inFolder = importPlaylist(*db, tmp.dir / "list.m3u8", folder);
    REQUIRE(inFolder.isOk());
    auto reader = std::move(db->openReader()).value();
    CHECK(PlaylistRepository(*reader).get(inFolder.value().playlistId)->parentId == folder);
}
