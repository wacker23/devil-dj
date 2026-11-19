// P2-09: importers from other software. Done when: sample XML fixture (hand-written) imports cues and
// playlists correctly. Fixtures: tests/data/fixtures/dj-library.xml and itunes-library.xml.

#include "TestData.h"
#include "library/Connection.h"
#include "library/CueRepository.h"
#include "library/Database.h"
#include "library/ForeignLibrary.h"
#include "library/PlaylistRepository.h"
#include "library/TrackRepository.h"
#include "library/XmlReader.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace devildj::library;
namespace fs = std::filesystem;

namespace
{
std::string utf8(const char8_t* t)
{
    return reinterpret_cast<const char*>(t);
}

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

fs::path fixture(const char* name)
{
    return fs::path(DEVILDJ_SOURCE_DIR) / "tests" / "data" / "fixtures" / name;
}

fs::path generated(const char* relative)
{
    return fs::path(devildj::test::generatedFile(relative).getFullPathName().toWideCharPointer());
}

std::string toUtf8(const fs::path& p)
{
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// A temp music folder with the fixture's files (missing.mp3 stays missing) and the fixture with {{DIR}} filled in.
struct Setup
{
    fs::path dir, music, xml;
    explicit Setup(const char* fixtureName)
    {
        dir = fs::temp_directory_path() /
              ("devildj-foreign-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        music = dir / "Music";
        const auto audio = readBytes(generated("formats/ref.mp3"));
        int n = 0;
        for (const auto& name : {fs::path("Opener.mp3"), fs::path("Second Track.mp3"), fs::path(u8"트랙.mp3")})
        {
            std::string id3(128, '\0'); // a unique tag: each file has its own content
            id3.replace(0, 3, "TAG");
            id3.replace(3, 1, std::string(1, static_cast<char>('a' + n++)));
            writeBytes(music / name, audio + id3);
        }
        auto text = readBytes(fixture(fixtureName));
        auto location = toUtf8(music);
        for (auto& c : location)
            if (c == '\\')
                c = '/';
        for (std::size_t at; (at = text.find("{{DIR}}")) != std::string::npos;)
            text.replace(at, 7, location);
        xml = dir / fixtureName;
        writeBytes(xml, text);
    }
    ~Setup()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

std::unique_ptr<Database> openDb(const fs::path& file)
{
    return std::move(Database::open(file)).value();
}
} // namespace

TEST_CASE("XML reader", "[library][xml]")
{
    const auto r = parseXml("\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<!DOCTYPE x [ <!ENTITY e \"no\"> ]>\n<!-- c -->"
                            "<root a=\"1 &amp; 2\" b='q&quot;'>text &lt;here&gt; &#65;&#x42;&#xD55C;"
                            "<empty/><child n=\"x\"><![CDATA[<raw> & stuff]]></child><!-- inner --><child n=\"y\"/>"
                            "</root>");
    REQUIRE(r.isOk());
    const auto& root = *r.value();
    CHECK(root.name == "root");
    CHECK(root.attributeOr("a") == "1 & 2");
    CHECK(root.attributeOr("b") == "q\"");
    CHECK(root.attributeOr("missing", "d") == "d");
    CHECK(root.text == "text <here> AB\xED\x95\x9C");
    CHECK(root.children.size() == 3);
    CHECK(root.child("empty") != nullptr);
    CHECK(root.child("child")->text == "<raw> & stuff");
    CHECK(root.childrenNamed("child").size() == 2);
    CHECK(root.childrenNamed("child")[1]->attributeOr("n") == "y");

    for (const char* bad : {"", "text", "<a>", "<a></b>", "<a x=1/>", "<a x=\"1/>", "<a>&nope;</a>", "<a/><b/>",
                            "<a>&#xZZ;</a>"})
    {
        INFO(bad);
        CHECK_FALSE(parseXml(bad).isOk());
    }
}

TEST_CASE("Locations in library files", "[library][xml]")
{
    CHECK(pathFromLocation("file://localhost/C:/My%20Music/a.mp3") == fs::path("C:\\My Music\\a.mp3"));
    CHECK(pathFromLocation("file:///C:/Music/b.mp3") == fs::path("C:\\Music\\b.mp3"));
    CHECK(pathFromLocation("file://localhost/D:/%ED%95%9C/c.mp3") == fs::path(u8"D:\\한\\c.mp3"));
    CHECK(pathFromLocation(utf8(u8"file://localhost/D:/한/c.mp3")) == fs::path(u8"D:\\한\\c.mp3")); // raw UTF-8 too
    CHECK(pathFromLocation("file://server/share/x.mp3") == fs::path("\\\\server\\share\\x.mp3"));
    CHECK(pathFromLocation("C:\\Plain\\d.mp3") == fs::path("C:\\Plain\\d.mp3"));
}

TEST_CASE("DJ library XML: the fixture is read completely", "[library][xml]")
{
    const auto text = readBytes(fixture("dj-library.xml"));
    REQUIRE_FALSE(text.empty());
    const auto r = parseDjPlaylistsXml(text);
    REQUIRE(r.isOk());
    const auto& lib = r.value();
    CHECK(lib.format == "DJ library XML");
    REQUIRE(lib.tracks.size() == 4);

    const auto& t1 = lib.tracks[0];
    CHECK(t1.key == "11");
    CHECK(t1.title == "Opener");
    CHECK(t1.bpm == 120.0);
    CHECK(t1.tonality == "Am");
    CHECK(t1.colour == 0xE0483C);
    CHECK(t1.sampleRate == 44100);
    CHECK(t1.dateAdded == 1580428800); // 2020-01-31 UTC
    REQUIRE(t1.tempos.size() == 2);
    CHECK(t1.tempos[1].startSeconds == 10.025);
    CHECK(t1.tempos[1].bpm == 124.0);
    REQUIRE(t1.cues.size() == 6);
    CHECK(t1.cues[0].kind == ForeignCue::Kind::Cue);
    CHECK(t1.cues[0].slot == 0);
    CHECK(t1.cues[0].label == "Drop");
    CHECK(t1.cues[0].colour == 0xE0483C);
    CHECK_FALSE(t1.cues[1].slot.has_value());
    CHECK(t1.cues[2].kind == ForeignCue::Kind::Loop);
    CHECK(t1.cues[2].label == "Build & break");
    CHECK(t1.cues[2].endSeconds == 6.0);
    CHECK(t1.cues[4].kind == ForeignCue::Kind::FadeIn);
    CHECK(t1.cues[5].kind == ForeignCue::Kind::FadeOut);

    const auto& t2 = lib.tracks[1];
    CHECK(t2.rating == 4); // 204 / 51
    CHECK(t2.bpm == 126.5);
    CHECK(t2.playCount == 7);
    CHECK(lib.tracks[2].title == utf8(u8"한국어 트랙"));
    CHECK(lib.tracks[2].rating == 5);
    CHECK_FALSE(lib.tracks[2].bpm.has_value()); // 0.00 = unknown

    // The tree under ROOT: Gigs/{Friday, By location}, Loose.
    REQUIRE(lib.playlists.size() == 2);
    CHECK(lib.playlists[0].name == "Gigs");
    CHECK(lib.playlists[0].folder);
    REQUIRE(lib.playlists[0].children.size() == 2);
    CHECK(lib.playlists[0].children[0].trackKeys == std::vector<std::string>{"12", "11", "14", "12"});
    CHECK(lib.playlists[0].children[1].trackKeys == std::vector<std::string>{"13"}); // by location
    CHECK(lib.playlists[1].name == "Loose");
    CHECK(lib.playlists[1].trackKeys.empty());

    CHECK_FALSE(parseDjPlaylistsXml("<plist/>").isOk());
}

TEST_CASE("DJ library XML: importing the fixture brings tracks, cues, grids and playlists", "[library][xml]")
{
    if (!fs::exists(generated("formats/ref.mp3")))
        SKIP("Generated test signals missing (python tools/gen_test_signals.py)");
    Setup setup("dj-library.xml");
    auto db = openDb(setup.dir / "library.db");
    const auto lib = readForeignLibrary(setup.xml);
    REQUIRE(lib.isOk());
    const auto r = importForeignLibrary(*db, lib.value(), "Imported");
    REQUIRE(r.isOk());
    const auto& res = r.value();
    CHECK(res.tracksInFile == 4);
    CHECK(res.added == 3);
    CHECK(res.matched == 3);
    CHECK(res.missingFiles == 1);
    CHECK(res.cuesAdded == 6);
    CHECK(res.beatgridsAdded == 1);
    CHECK(res.playlists == 3);

    auto reader = std::move(db->openReader()).value();
    TrackRepository tracks(*reader);
    const auto byFile = [&](const fs::path& name) { return *tracks.findByPath(libraryPath(setup.music / name)); };
    const auto opener = byFile("Opener.mp3");
    const auto second = byFile("Second Track.mp3");
    const auto korean = byFile(fs::path(u8"트랙.mp3"));

    // Metadata from the file.
    CHECK(opener.color == 0xE0483C);
    CHECK(opener.musicalKey == 21);
    CHECK(opener.bpm == 120.0);
    CHECK(opener.playCount == 2);
    CHECK(opener.dateAdded == 1580428800);
    CHECK(second.rating == 4);
    CHECK(second.color == 0x3C7CE0);
    CHECK(second.bpm == 126.5);
    CHECK(second.musicalKey == 21); // 8A
    CHECK(second.comment == "from the xml");
    CHECK(korean.rating == 5);

    // Cues: kinds, pads, positions in samples at the file's rate, loop lengths, labels and colours.
    const auto cues = CueRepository(*reader).forTrack(opener.id);
    REQUIRE(cues.size() == 6);
    const auto find = [&](CueKind kind) -> const Cue*
    {
        for (const auto& c : cues)
            if (c.kind == kind)
                return &c;
        return nullptr;
    };
    REQUIRE(find(CueKind::HotCue) != nullptr);
    CHECK(find(CueKind::HotCue)->slot == 0);
    CHECK(find(CueKind::HotCue)->startSample == 66150); // 1.5 s at 44.1 kHz
    CHECK(find(CueKind::HotCue)->label == "Drop");
    CHECK(find(CueKind::HotCue)->color == 0xE0483C);
    REQUIRE(find(CueKind::HotLoop) != nullptr);
    CHECK(find(CueKind::HotLoop)->slot == 1);
    CHECK(find(CueKind::HotLoop)->startSample == 176400);
    CHECK(find(CueKind::HotLoop)->lengthSamples == 88200);
    CHECK(find(CueKind::HotLoop)->label == "Build & break");
    CHECK(find(CueKind::MemoryCue)->startSample == 132300);
    CHECK(find(CueKind::MemoryLoop)->lengthSamples == 22050);
    CHECK(find(CueKind::MixIn)->startSample == 4410);
    CHECK(find(CueKind::MixOut) != nullptr);

    // The beatgrid from the two tempo markers (the second starts 20 beats in at 120 BPM).
    CHECK(reader->queryText("SELECT anchors_json FROM beatgrids WHERE track_id = " + std::to_string(opener.id)) ==
          "[{\"beat\":0,\"sample\":1103,\"bpm\":120.0000,\"bar_beat\":1},"
          "{\"beat\":20,\"sample\":442103,\"bpm\":124.0000,\"bar_beat\":1}]");

    // Playlists: inside the import folder, the same tree, the missing track left out.
    PlaylistRepository playlists(*reader);
    const auto top = playlists.get(res.folderId);
    REQUIRE(top.has_value());
    CHECK(top->name == "Imported");
    CHECK(top->kind == PlaylistKind::Folder);
    const auto inside = playlists.children(res.folderId);
    REQUIRE(inside.size() == 2);
    CHECK(inside[0].name == "Gigs");
    CHECK(inside[0].kind == PlaylistKind::Folder);
    CHECK(inside[1].name == "Loose");
    const auto gigs = playlists.children(inside[0].id);
    REQUIRE(gigs.size() == 2);
    CHECK(gigs[0].name == "Friday");
    CHECK(playlists.trackIds(gigs[0].id) == std::vector<std::int64_t>{second.id, opener.id, second.id});
    CHECK(gigs[1].name == "By location");
    CHECK(playlists.trackIds(gigs[1].id) == std::vector<std::int64_t>{korean.id});
    CHECK(playlists.trackIds(inside[1].id).empty());

    // Importing again: nothing new in the library, no duplicate cues or grids.
    const auto again = importForeignLibrary(*db, lib.value(), "Imported again");
    REQUIRE(again.isOk());
    CHECK(again.value().added == 0);
    CHECK(again.value().cuesAdded == 0);
    CHECK(again.value().beatgridsAdded == 0);
    CHECK(CueRepository(*reader).forTrack(opener.id).size() == 6);
    CHECK(reader->queryInt("SELECT count(*) FROM tracks") == 3);
}

TEST_CASE("iTunes library XML: the fixture's tracks and playlists", "[library][xml]")
{
    const auto parsed = parseITunesXml(readBytes(fixture("itunes-library.xml")));
    REQUIRE(parsed.isOk());
    const auto& lib = parsed.value();
    CHECK(lib.format == "iTunes library XML");
    REQUIRE(lib.tracks.size() == 3); // the stream is skipped
    CHECK(lib.tracks[0].key == "2001");
    CHECK(lib.tracks[0].rating == 4);    // 80 / 20
    CHECK(lib.tracks[1].rating == 0);    // a computed (album) rating isn't the track's
    CHECK(lib.tracks[0].bpm == 121.0);
    CHECK(lib.tracks[0].playCount == 12);
    CHECK(lib.tracks[0].comment == "iTunes & me");
    CHECK(lib.tracks[0].dateAdded == 1559420100); // 2019-06-01T20:15:00Z
    // Library and Music are left out; Night belongs in the Sets folder even though it comes first.
    REQUIRE(lib.playlists.size() == 2);
    CHECK(lib.playlists[0].name == "Sets");
    CHECK(lib.playlists[0].folder);
    REQUIRE(lib.playlists[0].children.size() == 1);
    CHECK(lib.playlists[0].children[0].name == "Night");
    CHECK(lib.playlists[0].children[0].trackKeys == std::vector<std::string>{"2002", "2003", "2001"});
    CHECK(lib.playlists[1].name == "Top");

    if (!fs::exists(generated("formats/ref.mp3")))
        SKIP("Generated test signals missing (python tools/gen_test_signals.py)");
    Setup setup("itunes-library.xml");
    auto db = openDb(setup.dir / "library.db");
    const auto lib2 = readForeignLibrary(setup.xml);
    REQUIRE(lib2.isOk());
    const auto r = importForeignLibrary(*db, lib2.value(), "From iTunes");
    REQUIRE(r.isOk());
    CHECK(r.value().added == 2);
    CHECK(r.value().missingFiles == 1);
    CHECK(r.value().playlists == 2);
    auto reader = std::move(db->openReader()).value();
    TrackRepository tracks(*reader);
    const auto opener = *tracks.findByPath(libraryPath(setup.music / "Opener.mp3"));
    const auto second = *tracks.findByPath(libraryPath(setup.music / "Second Track.mp3"));
    CHECK(opener.rating == 4);
    CHECK(opener.bpm == 121.0);
    CHECK(opener.comment == "iTunes & me");
    CHECK(opener.dateAdded == 1559420100);
    PlaylistRepository playlists(*reader);
    const auto sets = playlists.children(r.value().folderId).at(0);
    const auto night = playlists.children(sets.id).at(0);
    CHECK(playlists.trackIds(night.id) == std::vector<std::int64_t>{second.id, opener.id});
}

TEST_CASE("Library files: the format is picked from the file", "[library][xml]")
{
    const auto dir = fs::temp_directory_path() /
                     ("devildj-foreign-detect-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    writeBytes(dir / "a.xml", "<?xml version=\"1.0\"?><DJ_PLAYLISTS><COLLECTION/></DJ_PLAYLISTS>");
    writeBytes(dir / "b.xml", "<?xml version=\"1.0\"?><!DOCTYPE plist><plist><dict/></plist>");
    writeBytes(dir / "c.xml", "<?xml version=\"1.0\"?><svg/>");
    CHECK(readForeignLibrary(dir / "a.xml").value().format == "DJ library XML");
    CHECK(readForeignLibrary(dir / "b.xml").value().format == "iTunes library XML");
    CHECK_FALSE(readForeignLibrary(dir / "c.xml").isOk());
    CHECK_FALSE(readForeignLibrary(dir / "none.xml").isOk());
    std::error_code ec;
    fs::remove_all(dir, ec);
}
