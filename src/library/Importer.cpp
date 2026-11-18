#include "Importer.h"

#include "library/ContentHash.h"
#include "library/Database.h"
#include "library/TagReader.h"
#include "util/ThreadPriority.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <set>
#include <thread>
#include <unordered_set>

namespace devildj::library
{

namespace
{
constexpr std::size_t kMaxErrors = 50;

std::string asciiLower(std::string s)
{
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string toUtf8(const std::filesystem::path& p)
{
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// Windows paths are case-insensitive: compare them in lower case.
std::string pathKey(const std::string& storedPath)
{
    return asciiLower(storedPath);
}

bool hasExtension(const std::filesystem::path& p, const std::set<std::string>& extensions)
{
    return extensions.count(asciiLower(toUtf8(p.extension()))) > 0;
}

void addError(ImportResult& r, const std::string& path, const std::string& why)
{
    if (r.errors.size() < kMaxErrors)
        r.errors.push_back(path + ": " + why);
}

struct Item
{
    std::filesystem::path file;
    std::string path; // libraryPath(file)
    bool knownPath = false;
    ContentHash hash;
    TrackTags tags;
    std::string error;
};

std::int64_t nowSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool writeFile(const std::filesystem::path& file, const std::vector<std::uint8_t>& data)
{
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"wb") != 0 || f == nullptr)
        return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}
} // namespace

std::vector<std::string> ImportOptions::defaultAudioExtensions()
{
    return {".wav", ".wave", ".aif", ".aiff", ".aifc", ".flac", ".ogg", ".mp3", ".m4a", ".aac", ".mp4", ".alac"};
}

std::string libraryPath(const std::filesystem::path& file)
{
    std::error_code ec;
    auto absolute = std::filesystem::absolute(file, ec);
    if (ec)
        absolute = file;
    return toUtf8(absolute.lexically_normal().make_preferred());
}

std::vector<std::filesystem::path> findAudioFiles(std::span<const std::filesystem::path> inputs,
                                                  const std::vector<std::string>& extensionList,
                                                  std::vector<std::string>* errors)
{
    namespace fs = std::filesystem;
    std::set<std::string> extensions;
    for (const auto& e : extensionList)
        extensions.insert(asciiLower(e));

    std::vector<fs::path> files;
    std::unordered_set<std::string> seen;
    const auto add = [&](const fs::path& p)
    {
        if (!hasExtension(p, extensions))
            return;
        const auto stored = libraryPath(p);
        if (seen.insert(pathKey(stored)).second)
            files.push_back(fs::path(std::u8string(reinterpret_cast<const char8_t*>(stored.data()), stored.size())));
    };
    const auto report = [&](const fs::path& p, const std::error_code& ec)
    {
        if (errors != nullptr && errors->size() < kMaxErrors)
            errors->push_back(toUtf8(p) + ": " + ec.message());
    };

    for (const auto& input : inputs)
    {
        std::error_code ec;
        if (fs::is_directory(input, ec))
        {
            std::vector<fs::path> found;
            fs::recursive_directory_iterator it(input, fs::directory_options::skip_permission_denied, ec);
            if (ec)
            {
                report(input, ec);
                continue;
            }
            for (const fs::recursive_directory_iterator end; it != end; it.increment(ec))
            {
                if (ec)
                {
                    report(input, ec);
                    break;
                }
                std::error_code typeError;
                if (it->is_regular_file(typeError))
                    found.push_back(it->path());
            }
            std::sort(found.begin(), found.end());
            for (const auto& f : found)
                add(f);
        }
        else if (fs::is_regular_file(input, ec))
            add(input);
        else
            report(input, ec ? ec : std::make_error_code(std::errc::no_such_file_or_directory));
    }
    return files;
}

ImportResult importTracks(Database& db, std::span<const std::filesystem::path> inputs, const ImportOptions& options)
{
    const auto started = std::chrono::steady_clock::now();
    ImportResult result;
    const auto files = findAudioFiles(inputs, options.extensions, &result.errors);
    result.found = static_cast<int>(files.size());

    // What the library already has (one read at the start, then kept up to date as batches commit).
    std::unordered_set<std::string> knownPaths, knownHashes;
    {
        auto reader = db.openReader();
        if (!reader)
        {
            addError(result, toUtf8(db.file()), reader.error());
            result.failed = result.found;
            return result;
        }
        auto* s = reader.value()->prepare("SELECT path, content_hash FROM tracks");
        while (s != nullptr && s->step())
        {
            knownPaths.insert(pathKey(s->getText(0)));
            knownHashes.insert(s->getText(1));
        }
    }

    const bool storeArtwork = !options.artworkCacheDir.empty();
    if (storeArtwork)
    {
        std::error_code ec;
        std::filesystem::create_directories(options.artworkCacheDir, ec);
    }
    const int threads = options.threads > 0
                            ? options.threads
                            : std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 1, 1, 4);
    const std::size_t batch = static_cast<std::size_t>(std::max(1, options.batchSize));
    const int priority = util::currentThreadPriority();

    for (std::size_t first = 0; first < files.size(); first += batch)
    {
        if (options.isCancelled && options.isCancelled())
        {
            result.cancelled = true;
            break;
        }
        const std::size_t count = std::min(batch, files.size() - first);
        std::vector<Item> items(count);

        // Read the batch in parallel: hash + tags (the slow, I/O-bound part).
        std::atomic<std::size_t> next{0};
        const auto work = [&]
        {
            util::setCurrentThreadPriority(priority);
            for (std::size_t i = next++; i < count; i = next++)
            {
                auto& item = items[i];
                item.file = files[first + i];
                item.path = libraryPath(item.file);
                if (knownPaths.count(pathKey(item.path)) > 0) // read-only while the workers run
                {
                    item.knownPath = true;
                    continue;
                }
                auto hash = hashFileContent(item.file);
                if (!hash)
                {
                    item.error = hash.error();
                    continue;
                }
                item.hash = std::move(hash).value();
                if (knownHashes.count(item.hash.hex) > 0)
                    continue; // a duplicate: no need to read its tags
                item.tags = readTrackTags(item.file, storeArtwork);
                if (!item.tags.readable)
                    item.error = "not a readable audio file";
            }
        };
        {
            std::vector<std::jthread> pool;
            const int n = std::min<int>(threads, static_cast<int>(count));
            for (int t = 1; t < n; ++t)
                pool.emplace_back(work);
            work();
        }

        // Write the batch in one transaction on the writer thread.
        std::vector<std::pair<std::size_t, std::int64_t>> inserted; // item index, track id
        std::vector<std::pair<std::size_t, std::string>> artworkFiles;
        std::string writeError;
        const std::int64_t added = nowSeconds();
        db.writeAndWait(
            [&](Connection& c)
            {
                const bool ok = c.transaction(
                    [&]
                    {
                        for (std::size_t i = 0; i < count; ++i)
                        {
                            auto& item = items[i];
                            if (item.knownPath || !item.error.empty() || item.hash.hex.empty() ||
                                knownHashes.count(item.hash.hex) > 0 || !item.tags.readable)
                                continue;
                            auto* s = c.prepare(
                                "INSERT INTO tracks (path, content_hash, title, artist, album, genre, year, comment, "
                                "duration_ms, sample_rate, bitrate, file_size, bpm, musical_key, date_added) "
                                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
                            if (s == nullptr)
                            {
                                writeError = c.lastError();
                                return false;
                            }
                            const auto& t = item.tags;
                            const auto text = [](const std::string& v)
                            { return v.empty() ? std::optional<std::string_view>{} : std::optional<std::string_view>{v}; };
                            s->bind(1, item.path).bind(2, item.hash.hex).bind(3, text(t.title)).bind(4, text(t.artist));
                            s->bind(5, text(t.album)).bind(6, text(t.genre)).bind(7, t.year).bind(8, text(t.comment));
                            s->bind(9, t.durationMs).bind(10, t.sampleRate).bind(11, t.bitrateKbps);
                            s->bind(12, static_cast<std::int64_t>(item.hash.size));
                            s->bind(13, t.bpm > 0.0 ? std::optional<double>{t.bpm} : std::nullopt);
                            s->bind(14, t.musicalKey).bind(15, added);
                            if (!s->run())
                            {
                                writeError = s->error();
                                return false;
                            }
                            const auto id = c.lastInsertId();
                            inserted.emplace_back(i, id);
                            knownHashes.insert(item.hash.hex); // a second copy later in this import is a duplicate

                            if (storeArtwork && !t.artwork.empty())
                            {
                                const auto name = item.hash.hex + artworkExtension(t.artworkMimeType, t.artwork);
                                auto* a = c.prepare("INSERT OR REPLACE INTO analysis_artifacts (track_id, kind, path, "
                                                    "version) VALUES (?, 'artwork', ?, 1)");
                                if (a == nullptr || !a->bind(1, id).bind(2, name).run())
                                {
                                    writeError = a != nullptr ? a->error() : c.lastError();
                                    return false;
                                }
                                artworkFiles.emplace_back(i, name);
                            }
                        }
                        return true;
                    });
                if (!ok && writeError.empty())
                    writeError = c.lastError();
            });

        if (!writeError.empty())
        {
            // The whole batch rolled back: none of it was added. Undo the hashes this batch claimed.
            for (const auto& [i, id] : inserted)
                knownHashes.erase(items[i].hash.hex);
            for (const auto& item : items)
                if (!item.knownPath)
                {
                    ++result.failed;
                    addError(result, item.path, "database: " + writeError);
                }
            if (options.onProgress)
                options.onProgress(static_cast<int>(first + count), result.found);
            continue;
        }

        std::vector<bool> wasInserted(count, false);
        for (const auto& [i, id] : inserted)
        {
            wasInserted[i] = true;
            ++result.added;
            result.addedIds.push_back(id);
            knownPaths.insert(pathKey(items[i].path));
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto& item = items[i];
            if (wasInserted[i])
                continue;
            if (item.knownPath)
                ++result.alreadyInLibrary;
            else if (!item.error.empty())
            {
                ++result.failed;
                addError(result, item.path, item.error);
            }
            else
                ++result.duplicates;
        }
        for (const auto& [i, name] : artworkFiles)
        {
            const auto target = options.artworkCacheDir / name;
            std::error_code ec;
            if (!std::filesystem::exists(target, ec) && !writeFile(target, items[i].tags.artwork))
                addError(result, items[i].path, "couldn't save the cover art");
        }
        if (options.queueAnalysis)
            for (const auto& [i, id] : inserted)
                options.queueAnalysis(id);
        if (options.onProgress)
            options.onProgress(static_cast<int>(first + count), result.found);
    }

    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}

} // namespace devildj::library
