// P2-02: track import - key tags, content hash, full tag reading, folder scan, import / re-import, speed.

#include "TestData.h"
#include "engine/TrackDecoder.h"
#include "library/ContentHash.h"
#include "library/Database.h"
#include "library/Importer.h"
#include "library/TagReader.h"

#include <catch2/catch_test_macros.hpp>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/tvariant.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace devildj::library;
namespace fs = std::filesystem;

namespace
{
struct TempFolder
{
    fs::path dir;
    explicit TempFolder(const char* name)
    {
        dir = fs::temp_directory_path() /
              (std::string("devildj-import-") + name + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
    }
    ~TempFolder()
    {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

std::string utf8(const char8_t* text)
{
    return reinterpret_cast<const char*>(text);
}

fs::path generated(const char* relative)
{
    return fs::path(devildj::test::generatedFile(relative).getFullPathName().toWideCharPointer());
}

void writeBytes(const fs::path& file, const std::string& bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void copyTo(const fs::path& source, const fs::path& target)
{
    fs::create_directories(target.parent_path());
    fs::copy_file(source, target);
}

std::string readBytes(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// A copy of `source` with a 128-byte ID3v1 tag appended, so every copy has different content.
void copyWithId3v1(const fs::path& source, const fs::path& target, const std::string& title)
{
    std::string bytes = readBytes(source);
    std::string tag(128, '\0');
    tag.replace(0, 3, "TAG");
    tag.replace(3, std::min<std::size_t>(30, title.size()), title.substr(0, 30));
    bytes += tag;
    writeBytes(target, bytes);
}

const std::string kPng = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(64, '\x42');

void tagFile(const fs::path& file, const std::map<std::string, std::string>& props, bool withPicture)
{
    TagLib::FileRef ref(file.c_str());
    REQUIRE_FALSE(ref.isNull());
    auto properties = ref.properties();
    for (const auto& [k, v] : props)
        properties.replace(k, TagLib::StringList(TagLib::String(v, TagLib::String::UTF8)));
    ref.setProperties(properties);
    if (withPicture)
    {
        TagLib::VariantMap picture;
        picture["data"] = TagLib::ByteVector(kPng.data(), static_cast<unsigned>(kPng.size()));
        picture["mimeType"] = TagLib::String("image/png");
        picture["pictureType"] = TagLib::String("Front Cover");
        picture["description"] = TagLib::String("cover");
        REQUIRE(ref.setComplexProperties("PICTURE", {picture}));
    }
    REQUIRE(ref.save());
}

bool haveSignals()
{
    return fs::exists(generated("formats/ref.mp3"));
}

struct Row
{
    std::string path, title, artist, album, genre, comment, hash;
    std::optional<std::int64_t> year, durationMs, sampleRate, bitrate, fileSize, key;
    std::optional<double> bpm;
    std::int64_t analysisVersion = -1, dateAdded = 0;
};

std::vector<Row> rows(Database& db)
{
    auto reader = db.openReader();
    REQUIRE(reader.isOk());
    auto* s = reader.value()->prepare(
        "SELECT path, title, artist, album, genre, comment, content_hash, year, duration_ms, sample_rate, bitrate, "
        "file_size, musical_key, bpm, analysis_version, date_added FROM tracks ORDER BY path");
    REQUIRE(s != nullptr);
    std::vector<Row> out;
    while (s->step())
    {
        Row r;
        r.path = s->getText(0);
        r.title = s->getText(1);
        r.artist = s->getText(2);
        r.album = s->getText(3);
        r.genre = s->getText(4);
        r.comment = s->getText(5);
        r.hash = s->getText(6);
        r.year = s->getOptionalInt(7);
        r.durationMs = s->getOptionalInt(8);
        r.sampleRate = s->getOptionalInt(9);
        r.bitrate = s->getOptionalInt(10);
        r.fileSize = s->getOptionalInt(11);
        r.key = s->getOptionalInt(12);
        r.bpm = s->getOptionalDouble(13);
        r.analysisVersion = s->getInt(14);
        r.dateAdded = s->getInt(15);
        out.push_back(r);
    }
    return out;
}

std::string toUtf8(const fs::path& p)
{
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}
} // namespace

TEST_CASE("Key tags: note names, Camelot and Open Key", "[library][import]")
{
    // 0..11 = C..B major, 12..23 = C..B minor.
    CHECK(parseKeyTag("C") == 0);
    CHECK(parseKeyTag("Am") == 21);
    CHECK(parseKeyTag("A minor") == 21);
    CHECK(parseKeyTag("F#m") == 18);
    CHECK(parseKeyTag("Gbm") == 18);
    CHECK(parseKeyTag("Bbmin") == 22);
    CHECK(parseKeyTag("Ebmaj") == 3);
    CHECK(parseKeyTag("C#M") == 1);
    CHECK(parseKeyTag("Bm") == 23);
    CHECK(parseKeyTag("B") == 11);
    CHECK(parseKeyTag("E\xE2\x99\xADm") == 15); // E♭m
    CHECK(parseKeyTag("F\xE2\x99\xAF") == 6);   // F♯
    CHECK(parseKeyTag(" d ") == 2);

    CHECK(parseKeyTag("8B") == 0);   // C major
    CHECK(parseKeyTag("8A") == 21);  // A minor
    CHECK(parseKeyTag("9B") == 7);   // G major
    CHECK(parseKeyTag("9A") == 16);  // E minor
    CHECK(parseKeyTag("7B") == 5);   // F major
    CHECK(parseKeyTag("7A") == 14);  // D minor
    CHECK(parseKeyTag("12a") == 13); // C# minor
    CHECK(parseKeyTag("1B") == 11);  // B major

    CHECK(parseKeyTag("1d") == 0);  // C major
    CHECK(parseKeyTag("1m") == 21); // A minor
    CHECK(parseKeyTag("2d") == 7);  // G major
    CHECK(parseKeyTag("12d") == 5); // F major

    for (const char* bad : {"", "o", "H", "13A", "0B", "8C", "Am7 groove", "unknown"})
    {
        INFO(bad);
        CHECK_FALSE(parseKeyTag(bad).has_value());
    }

    // Every Camelot code maps to a distinct key, and Camelot and Open Key agree.
    std::set<int> seen;
    for (int n = 1; n <= 12; ++n)
        for (const char* letter : {"A", "B"})
        {
            const auto k = parseKeyTag(std::to_string(n) + letter);
            REQUIRE(k.has_value());
            seen.insert(*k);
            const int open = (n + 4) % 12 + 1; // Camelot 8 = Open Key 1
            CHECK(parseKeyTag(std::to_string(open) + (letter[0] == 'A' ? "m" : "d")) == k);
        }
    CHECK(seen.size() == 24);
}

TEST_CASE("Artwork file extension from MIME type or bytes", "[library][import]")
{
    CHECK(artworkExtension("image/jpeg", {}) == ".jpg");
    CHECK(artworkExtension("IMAGE/PNG", {}) == ".png");
    CHECK(artworkExtension("", {0xFF, 0xD8, 0xFF, 0xE0}) == ".jpg");
    CHECK(artworkExtension("", {0x89, 'P', 'N', 'G', 13, 10}) == ".png");
    CHECK(artworkExtension("image/x-weird", {1, 2, 3}) == ".img");
}

TEST_CASE("Content hash: size + first and last 1 MB", "[library][import]")
{
    TempFolder tmp("hash");
    const std::string small = "hello devil-dj";
    writeBytes(tmp.dir / "a.bin", small);
    writeBytes(tmp.dir / "b.bin", small);
    writeBytes(tmp.dir / "c.bin", small + "!");

    const auto a = hashFileContent(tmp.dir / "a.bin");
    REQUIRE(a.isOk());
    CHECK(a.value().hex.size() == 16);
    CHECK(a.value().size == small.size());
    CHECK(hashFileContent(tmp.dir / "b.bin").value().hex == a.value().hex);
    CHECK(hashFileContent(tmp.dir / "c.bin").value().hex != a.value().hex);

    // 3 MB: the middle MB isn't read; the first and last MB and the size are.
    std::string big(3u << 20, '\0');
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = static_cast<char>((i * 31) & 0xFF);
    writeBytes(tmp.dir / "big.bin", big);
    const auto base = hashFileContent(tmp.dir / "big.bin").value().hex;
    auto changed = big;
    changed[(3u << 20) / 2] ^= 1;
    writeBytes(tmp.dir / "middle.bin", changed);
    CHECK(hashFileContent(tmp.dir / "middle.bin").value().hex == base);
    changed = big;
    changed[100] ^= 1;
    writeBytes(tmp.dir / "start.bin", changed);
    CHECK(hashFileContent(tmp.dir / "start.bin").value().hex != base);
    changed = big;
    changed.back() ^= 1;
    writeBytes(tmp.dir / "end.bin", changed);
    CHECK(hashFileContent(tmp.dir / "end.bin").value().hex != base);
    writeBytes(tmp.dir / "longer.bin", big + std::string(1, '\0'));
    CHECK(hashFileContent(tmp.dir / "longer.bin").value().hex != base);

    CHECK_FALSE(hashFileContent(tmp.dir / "missing.bin").isOk());
}

TEST_CASE("Tags: the full set the library stores, with cover art", "[library][import]")
{
    if (!haveSignals())
        SKIP("Generated test signals missing (python tools/gen_test_signals.py)");
    TempFolder tmp("tags");
    const auto file = tmp.dir / fs::path(u8"트랙 01.mp3");
    fs::copy_file(generated("formats/ref.mp3"), file);
    tagFile(file,
            {{"TITLE", "Night Drive"},
             {"ARTIST", utf8(u8"김서연")},
             {"ALBUM", "After Hours"},
             {"GENRE", "Deep House"},
             {"DATE", "2024"},
             {"COMMENT", "warm-up"},
             {"BPM", "122"},
             {"INITIALKEY", "8A"}},
            true);

    const auto t = readTrackTags(file, true);
    CHECK(t.readable);
    CHECK(t.title == "Night Drive");
    CHECK(t.artist == utf8(u8"김서연"));
    CHECK(t.album == "After Hours");
    CHECK(t.genre == "Deep House");
    CHECK(t.year == 2024);
    CHECK(t.comment == "warm-up");
    CHECK(t.bpm == 122.0);
    CHECK(t.keyText == "8A");
    CHECK(t.musicalKey == 21);
    REQUIRE(t.durationMs.has_value());
    CHECK(*t.durationMs > 500);
    CHECK(t.sampleRate.has_value());
    CHECK(t.bitrateKbps.has_value());
    CHECK(t.artworkMimeType == "image/png");
    CHECK(std::string(t.artwork.begin(), t.artwork.end()) == kPng);

    const auto noArt = readTrackTags(file, false);
    CHECK(noArt.artwork.empty());
    CHECK(noArt.title == "Night Drive");

    writeBytes(tmp.dir / "fake.mp3", "this is not audio");
    CHECK_FALSE(readTrackTags(tmp.dir / "fake.mp3", true).readable);
}

TEST_CASE("Finding audio files: recursive, by extension, no repeats", "[library][import]")
{
    TempFolder tmp("scan");
    writeBytes(tmp.dir / "a.mp3", "x");
    writeBytes(tmp.dir / "B.FLAC", "x");
    writeBytes(tmp.dir / "notes.txt", "x");
    writeBytes(tmp.dir / "cover.jpg", "x");
    writeBytes(tmp.dir / "sub" / "deeper" / "c.wav", "x");
    writeBytes(tmp.dir / fs::path(u8"폴더") / "d.m4a", "x");

    const std::vector<fs::path> inputs{tmp.dir, tmp.dir / "a.mp3", tmp.dir / "sub", tmp.dir / "missing"};
    std::vector<std::string> errors;
    const auto files = findAudioFiles(inputs, ImportOptions::defaultAudioExtensions(), &errors);
    std::set<std::string> names;
    for (const auto& f : files)
    {
        CHECK(f.is_absolute());
        names.insert(toUtf8(f.filename()));
    }
    CHECK(files.size() == 4);
    CHECK(names == std::set<std::string>{"a.mp3", "B.FLAC", "c.wav", "d.m4a"});
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("missing") != std::string::npos);
}

TEST_CASE("The importer's extensions are the decoder's", "[library][import]")
{
    std::set<std::string> decoder;
    for (const auto& e : devildj::engine::supportedTrackExtensions())
        decoder.insert(e.toStdString());
    const auto list = ImportOptions::defaultAudioExtensions();
    CHECK(std::set<std::string>(list.begin(), list.end()) == decoder);
}

TEST_CASE("Import: adds tagged tracks once, skips duplicates, stores cover art", "[library][import]")
{
    if (!haveSignals())
        SKIP("Generated test signals missing (python tools/gen_test_signals.py)");
    TempFolder tmp("import");
    const auto music = tmp.dir / "Music";
    const auto source = generated("formats/ref.mp3");
    for (int i = 0; i < 4; ++i)
        copyWithId3v1(source, music / ("track" + std::to_string(i) + ".mp3"), "Track " + std::to_string(i));
    copyTo(generated("formats/ref.flac"), music / "Sub" / "flac.flac");
    tagFile(music / "track0.mp3", {{"TITLE", "Opener"}, {"ARTIST", "DJ Test"}, {"INITIALKEY", "Am"}, {"BPM", "124"}},
            true);
    copyTo(music / "track0.mp3", music / "~Copies" / "track0 copy.mp3"); // found after the original
    writeBytes(music / "broken.mp3", "not audio at all");
    writeBytes(music / "readme.txt", "ignore me");

    auto opened = Database::open(tmp.dir / "library.db");
    REQUIRE(opened.isOk());
    auto& db = *opened.value();

    std::vector<std::int64_t> queued;
    std::vector<std::pair<int, int>> progress;
    ImportOptions options;
    options.artworkCacheDir = tmp.dir / "cache";
    options.batchSize = 3;
    options.queueAnalysis = [&](std::int64_t id) { queued.push_back(id); };
    options.onProgress = [&](int done, int total) { progress.emplace_back(done, total); };

    const std::vector<fs::path> inputs{music};
    const auto first = importTracks(db, inputs, options);
    CHECK(first.found == 7); // 4 mp3 + flac + copy + broken
    CHECK(first.added == 5);
    CHECK(first.duplicates == 1);
    CHECK(first.failed == 1);
    CHECK(first.alreadyInLibrary == 0);
    CHECK_FALSE(first.cancelled);
    REQUIRE(first.errors.size() == 1);
    CHECK(first.errors[0].find("broken.mp3") != std::string::npos);
    CHECK(queued == first.addedIds);
    REQUIRE_FALSE(progress.empty());
    CHECK(progress.back() == std::pair<int, int>{7, 7});

    const auto library = rows(db);
    REQUIRE(library.size() == 5);
    const Row* opener = nullptr;
    for (const auto& r : library)
    {
        CHECK(fs::path(std::u8string(reinterpret_cast<const char8_t*>(r.path.data()), r.path.size())).is_absolute());
        CHECK(r.path.find('/') == std::string::npos);
        CHECK(r.hash.size() == 16);
        CHECK(r.analysisVersion == 0); // waiting for analysis
        CHECK(r.dateAdded > 1700000000);
        CHECK(r.durationMs.value_or(0) > 500);
        CHECK(r.fileSize.value_or(0) > 1000);
        if (r.title == "Opener")
            opener = &r;
    }
    REQUIRE(opener != nullptr);
    CHECK(opener->artist == "DJ Test");
    CHECK(opener->key == 21);
    CHECK(opener->bpm == 124.0);
    CHECK(opener->path.find("Copies") == std::string::npos); // the first one found was kept

    // Cover art: one file named after the content hash, and its artifact row.
    const auto art = tmp.dir / "cache" / (opener->hash + ".png");
    CHECK(readBytes(art) == kPng);
    {
        auto reader = db.openReader();
        REQUIRE(reader.isOk());
        CHECK(reader.value()->queryInt("SELECT count(*) FROM analysis_artifacts WHERE kind = 'artwork'") == 1);
        CHECK(reader.value()->queryText("SELECT path FROM analysis_artifacts WHERE kind = 'artwork'") ==
              opener->hash + ".png");
    }

    // Importing again adds nothing - also with a file given twice and in other letter case.
    queued.clear();
    const std::vector<fs::path> again{music, music / "TRACK1.MP3"};
    const auto second = importTracks(db, again, options);
    CHECK(second.added == 0);
    CHECK(second.alreadyInLibrary == 5);
    CHECK(second.duplicates == 1);
    CHECK(second.failed == 1);
    CHECK(queued.empty());
    CHECK(rows(db).size() == 5);

    // A moved / renamed copy of a library track is a duplicate, not a new track.
    copyTo(music / "track2.mp3", tmp.dir / "Elsewhere" / "renamed.mp3");
    const std::vector<fs::path> moved{tmp.dir / "Elsewhere"};
    const auto third = importTracks(db, moved, options);
    CHECK(third.added == 0);
    CHECK(third.duplicates == 1);
}

TEST_CASE("Import: can be cancelled between batches", "[library][import]")
{
    if (!haveSignals())
        SKIP("Generated test signals missing (python tools/gen_test_signals.py)");
    TempFolder tmp("cancel");
    for (int i = 0; i < 10; ++i)
        copyWithId3v1(generated("formats/ref.mp3"), tmp.dir / "m" / (std::to_string(i) + ".mp3"), std::to_string(i));
    auto db = Database::open(tmp.dir / "library.db");
    REQUIRE(db.isOk());
    ImportOptions options;
    options.batchSize = 4;
    bool stop = false;
    options.onProgress = [&](int, int) { stop = true; };
    options.isCancelled = [&] { return stop; };
    const std::vector<fs::path> inputs{tmp.dir / "m"};
    const auto r = importTracks(*db.value(), inputs, options);
    CHECK(r.cancelled);
    CHECK(r.added == 4);
    CHECK(rows(*db.value()).size() == 4);

    // Running it again picks up the rest.
    options.isCancelled = nullptr;
    const auto rest = importTracks(*db.value(), inputs, options);
    CHECK(rest.added == 6);
    CHECK(rest.alreadyInLibrary == 4);
}

TEST_CASE("Import: 2,000 files in under 60 s; re-import adds nothing", "[library][import]")
{
    if (!haveSignals())
        SKIP("Generated test signals missing (python tools/gen_test_signals.py)");
    TempFolder tmp("speed");
    const auto source = generated("formats/ref.mp3");
    const auto bytes = readBytes(source);
    constexpr int kFiles = 2000;
    for (int i = 0; i < kFiles; ++i)
    {
        std::string tag(128, '\0');
        tag.replace(0, 3, "TAG");
        const auto title = "Track " + std::to_string(i);
        tag.replace(3, title.size(), title);
        writeBytes(tmp.dir / "Music" / ("Folder " + std::to_string(i / 100)) / (std::to_string(i) + ".mp3"),
                   bytes + tag);
    }
    auto db = Database::open(tmp.dir / "library.db");
    REQUIRE(db.isOk());
    const std::vector<fs::path> inputs{tmp.dir / "Music"};

    const auto first = importTracks(*db.value(), inputs);
    WARN("Imported " << first.added << " files in " << first.seconds << " s");
    CHECK(first.found == kFiles);
    CHECK(first.added == kFiles);
    CHECK(first.failed == 0);
    CHECK(first.seconds < 60.0);

    const auto again = importTracks(*db.value(), inputs);
    WARN("Re-import: " << again.seconds << " s");
    CHECK(again.added == 0);
    CHECK(again.alreadyInLibrary == kFiles);
    CHECK(again.seconds < 10.0);
    CHECK(rows(*db.value()).size() == static_cast<std::size_t>(kFiles));
}

// Hidden (run by hand: DevilDJTests.exe "[.perf]"): 2,000 full-size MP3s (the 10-minute test file, 4.2 MB each,
// 8.4 GB in all), so hashing reads real amounts of data. Needs ~9 GB free in %TEMP%. Files just written are in the
// OS cache, so this is the warm-cache number; a cold first import of a real folder is slower (see DEVLOG).
TEST_CASE("Import speed with full-size files", "[library][import][.perf]")
{
    const auto source = generated("long_10min.mp3");
    const auto alt = generated("formats/long_10min.mp3");
    const auto file = fs::exists(source) ? source : alt;
    if (!fs::exists(file))
        SKIP("Generated test signals missing (python tools/gen_test_signals.py)");
    TempFolder tmp("speed-full");
    const auto bytes = readBytes(file);
    constexpr int kFiles = 2000;
    const auto writeStart = std::chrono::steady_clock::now();
    for (int i = 0; i < kFiles; ++i)
    {
        std::string tag(128, '\0');
        tag.replace(0, 3, "TAG");
        const auto title = "Track " + std::to_string(i);
        tag.replace(3, title.size(), title);
        auto target = tmp.dir / "Music" / ("Folder " + std::to_string(i / 100)) / (std::to_string(i) + ".mp3");
        fs::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.write(tag.data(), static_cast<std::streamsize>(tag.size()));
    }
    WARN("Wrote " << kFiles << " x " << bytes.size() / 1024 << " KB in "
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - writeStart).count() << " s");

    auto db = Database::open(tmp.dir / "library.db");
    REQUIRE(db.isOk());
    const std::vector<fs::path> inputs{tmp.dir / "Music"};
    const auto first = importTracks(*db.value(), inputs);
    WARN("Imported " << first.added << " full-size files in " << first.seconds << " s");
    CHECK(first.added == kFiles);
    CHECK(first.seconds < 60.0);
    const auto again = importTracks(*db.value(), inputs);
    WARN("Re-import: " << again.seconds << " s, added " << again.added);
    CHECK(again.added == 0);
}
