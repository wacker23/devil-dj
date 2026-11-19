#include "PlaylistFiles.h"

#include "library/Connection.h"
#include "library/ContentHash.h"
#include "library/Database.h"
#include "library/PlaylistRepository.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

namespace devildj::library
{

namespace
{
std::string toUtf8(const std::filesystem::path& p)
{
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::filesystem::path fromUtf8(const std::string& s)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

bool isValidUtf8(const std::string& s)
{
    std::size_t i = 0;
    while (i < s.size())
    {
        const auto c = static_cast<unsigned char>(s[i]);
        const int extra = c < 0x80 ? 0 : (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : (c >> 3) == 0x1E ? 3 : -1;
        if (extra < 0 || i + static_cast<std::size_t>(extra) >= s.size() + (extra == 0 ? 1 : 0))
            return false;
        for (int k = 1; k <= extra; ++k)
            if ((static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]) >> 6) != 0x2)
                return false;
        i += static_cast<std::size_t>(extra) + 1;
    }
    return true;
}

std::string ansiToUtf8(const std::string& s)
{
    if (s.empty())
        return s;
    const int wide = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(wide), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), w.data(), wide);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wide, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::string trim(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

std::string percentDecode(const std::string& s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2])))
        {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        }
        else
        {
            out += s[i];
        }
    }
    return out;
}

std::string lowerExtension(const std::filesystem::path& p)
{
    auto e = toUtf8(p.extension());
    for (auto& c : e)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return e;
}
} // namespace

bool isPlaylistFile(const std::filesystem::path& file)
{
    const auto e = lowerExtension(file);
    return e == ".m3u" || e == ".m3u8";
}

bool writeM3u8(const std::filesystem::path& file, const std::vector<M3uEntry>& entries)
{
    std::string text = "#EXTM3U\r\n";
    for (const auto& e : entries)
    {
        text += "#EXTINF:" + std::to_string(e.seconds) + "," + e.title + "\r\n";
        text += libraryPath(e.file) + "\r\n";
    }
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"wb") != 0 || f == nullptr)
        return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

util::Result<std::vector<M3uEntry>> readM3u(const std::filesystem::path& file)
{
    using R = util::Result<std::vector<M3uEntry>>;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return R::fail("Can't open the playlist file");
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    if (bytes.rfind("\xEF\xBB\xBF", 0) == 0)
        bytes.erase(0, 3);
    else if (!isValidUtf8(bytes))
        bytes = ansiToUtf8(bytes); // an old .m3u in the Windows code page

    std::vector<M3uEntry> entries;
    std::string pendingTitle;
    int pendingSeconds = -1;
    const auto folder = file.parent_path();
    std::istringstream lines(bytes);
    for (std::string line; std::getline(lines, line);)
    {
        line = trim(line);
        if (line.empty())
            continue;
        if (line[0] == '#')
        {
            if (line.rfind("#EXTINF:", 0) == 0)
            {
                const auto info = line.substr(8);
                const auto comma = info.find(',');
                pendingSeconds = std::atoi(info.substr(0, comma).c_str());
                pendingTitle = comma == std::string::npos ? std::string() : trim(info.substr(comma + 1));
            }
            continue;
        }
        if (line.rfind("file:///", 0) == 0)
        {
            line = percentDecode(line.substr(8));
            for (auto& c : line)
                if (c == '/')
                    c = '\\';
        }
        auto path = fromUtf8(line);
        if (!path.is_absolute())
            path = folder / path; // relative to the playlist ("\Music\a.mp3" keeps the drive)
        entries.push_back({path.lexically_normal(), pendingTitle, pendingSeconds});
        pendingTitle.clear();
        pendingSeconds = -1;
    }
    return R::ok(std::move(entries));
}

util::Result<int> exportPlaylist(Connection& reader, std::int64_t playlistId, const std::filesystem::path& file)
{
    using R = util::Result<int>;
    auto* s = reader.prepare("SELECT t.path, t.title, t.artist, t.duration_ms FROM playlist_tracks pt "
                             "JOIN tracks t ON t.id = pt.track_id WHERE pt.playlist_id = ? ORDER BY pt.position");
    if (s == nullptr)
        return R::fail(reader.lastError());
    s->bind(1, playlistId);
    std::vector<M3uEntry> entries;
    while (s->step())
    {
        M3uEntry e;
        e.file = fromUtf8(s->getText(0));
        const auto title = s->getText(1), artist = s->getText(2);
        e.title = artist.empty() ? title : title.empty() ? artist : artist + " - " + title;
        if (const auto ms = s->getOptionalInt(3))
            e.seconds = static_cast<int>((*ms + 500) / 1000);
        entries.push_back(std::move(e));
    }
    if (!s->ok())
        return R::fail(s->error());
    if (!writeM3u8(file, entries))
        return R::fail("Can't write " + toUtf8(file));
    return R::ok(static_cast<int>(entries.size()));
}

util::Result<PlaylistImportResult> importPlaylist(Database& db, const std::filesystem::path& file,
                                                  std::optional<std::int64_t> parentFolder,
                                                  const ImportOptions& options)
{
    using R = util::Result<PlaylistImportResult>;
    auto entries = readM3u(file);
    if (!entries)
        return R::fail(entries.error());

    PlaylistImportResult result;
    result.name = toUtf8(file.stem());
    std::vector<std::filesystem::path> present;
    for (const auto& e : entries.value())
    {
        std::error_code ec;
        if (std::filesystem::is_regular_file(e.file, ec))
            present.push_back(e.file);
        else
            result.missing.push_back(toUtf8(e.file));
    }

    // Files not in the library yet are added first (repeats and known paths are skipped by the importer).
    if (!present.empty())
    {
        auto importOptions = options;
        importOptions.onProgress = nullptr;
        result.addedToLibrary = importTracks(db, present, importOptions).added;
    }

    // Track ids in playlist order: by path, else by content (the same file under another path).
    auto opened = db.openReader();
    if (!opened)
        return R::fail(opened.error());
    auto& reader = *opened.value();
    std::vector<std::int64_t> ids;
    for (const auto& f : present)
    {
        std::optional<std::int64_t> id;
        if (auto* s = reader.prepare("SELECT id FROM tracks WHERE path = ? COLLATE NOCASE"))
        {
            if (s->bind(1, libraryPath(f)).step())
                id = s->getInt(0);
            s->reset();
        }
        if (!id)
            if (const auto hash = hashFileContent(f))
                if (auto* s = reader.prepare("SELECT id FROM tracks WHERE content_hash = ? LIMIT 1"))
                {
                    if (s->bind(1, hash.value().hex).step())
                        id = s->getInt(0);
                    s->reset();
                }
        if (id)
            ids.push_back(*id);
        else
            result.missing.push_back(toUtf8(f) + " (not an audio file the library can read)");
    }

    std::string error;
    db.writeAndWait(
        [&](Connection& c)
        {
            c.transaction(
                [&]
                {
                    PlaylistRepository repo(c);
                    Playlist p;
                    p.parentId = parentFolder;
                    p.name = result.name.empty() ? "Imported playlist" : result.name;
                    const auto created = repo.create(p);
                    if (!created)
                    {
                        error = created.error();
                        return false;
                    }
                    result.playlistId = created.value();
                    if (!repo.setTracks(result.playlistId, ids))
                    {
                        error = repo.error();
                        return false;
                    }
                    return true;
                });
        });
    if (!error.empty())
        return R::fail(error);
    result.tracks = static_cast<int>(ids.size());
    return R::ok(std::move(result));
}

} // namespace devildj::library
