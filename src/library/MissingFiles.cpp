#include "MissingFiles.h"

#include "library/Connection.h"
#include "library/ContentHash.h"
#include "library/Database.h"
#include "library/Importer.h"

#include <algorithm>
#include <optional>
#include <set>
#include <unordered_map>

namespace devildj::library
{

namespace
{
constexpr std::size_t kMaxNotes = 50;

std::filesystem::path fromUtf8(const std::string& s)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string toUtf8(const std::filesystem::path& p)
{
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::string lowerName(const std::filesystem::path& p)
{
    auto name = toUtf8(p.filename());
    for (auto& c : name)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return name;
}

struct TrackFile
{
    std::int64_t id = 0;
    std::string path, hash;
    std::int64_t size = -1;
    bool missing = false;
};
} // namespace

void setTrackMissing(Database& db, std::int64_t trackId, bool missing)
{
    db.write(
        [trackId, missing](Connection& c)
        {
            if (auto* s = c.prepare("UPDATE tracks SET missing = ? WHERE id = ? AND missing <> ?"))
                if (s->bind(1, missing ? 1 : 0).bind(2, trackId).bind(3, missing ? 1 : 0).run() && c.changes() > 0)
                    c.noteChange(Table::Tracks, trackId);
        });
}

MissingScanResult scanForMissingFiles(Database& db, const std::function<bool()>& isCancelled,
                                      const std::function<void(int, int)>& onProgress)
{
    MissingScanResult result;
    std::vector<TrackFile> tracks;
    {
        auto reader = db.openReader();
        if (!reader)
            return result;
        auto* s = reader.value()->prepare("SELECT id, path, missing FROM tracks");
        while (s != nullptr && s->step())
            tracks.push_back({s->getInt(0), s->getText(1), {}, -1, s->getInt(2) != 0});
    }

    std::vector<std::pair<std::int64_t, bool>> changes; // id, missing now
    const int total = static_cast<int>(tracks.size());
    for (int i = 0; i < total; ++i)
    {
        if (isCancelled && (i % 256) == 0 && isCancelled())
        {
            result.cancelled = true;
            break;
        }
        const auto& t = tracks[static_cast<std::size_t>(i)];
        std::error_code ec;
        const bool gone = !std::filesystem::is_regular_file(fromUtf8(t.path), ec);
        ++result.checked;
        if (gone)
            ++result.missing;
        if (gone != t.missing)
        {
            changes.emplace_back(t.id, gone);
            ++(gone ? result.newlyMissing : result.foundAgain);
        }
        if (onProgress && (i % 1000) == 999)
            onProgress(i + 1, total);
    }

    if (!changes.empty())
        db.writeAndWait(
            [&](Connection& c)
            {
                c.transaction(
                    [&]
                    {
                        auto* s = c.prepare("UPDATE tracks SET missing = ? WHERE id = ?");
                        if (s == nullptr)
                            return false;
                        for (const auto& [id, gone] : changes)
                        {
                            if (!s->bind(1, gone ? 1 : 0).bind(2, id).run())
                                return false;
                            c.noteChange(Table::Tracks, id);
                        }
                        return true;
                    });
            });
    if (onProgress)
        onProgress(total, total);
    return result;
}

RelocateResult relocateMissingFiles(Database& db, const std::filesystem::path& folder,
                                    std::span<const std::int64_t> trackIds, const std::function<bool()>& isCancelled)
{
    RelocateResult result;
    const auto note = [&](std::string text)
    {
        if (result.notes.size() < kMaxNotes)
            result.notes.push_back(std::move(text));
    };

    // The tracks to look for: missing ones (or the given ones whose file isn't there).
    std::vector<TrackFile> wanted;
    std::set<std::string> takenPaths; // paths already in the library (lower case)
    {
        auto reader = db.openReader();
        if (!reader)
            return result;
        const std::set<std::int64_t> only(trackIds.begin(), trackIds.end());
        auto* s = reader.value()->prepare("SELECT id, path, content_hash, file_size, missing FROM tracks");
        while (s != nullptr && s->step())
        {
            TrackFile t{s->getInt(0), s->getText(1), s->getText(2), s->getOptionalInt(3).value_or(-1),
                        s->getInt(4) != 0};
            auto key = t.path;
            for (auto& c : key)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            takenPaths.insert(key);
            std::error_code ec;
            const bool gone = t.missing || !std::filesystem::is_regular_file(fromUtf8(t.path), ec);
            if (gone && (only.empty() || only.count(t.id) > 0))
                wanted.push_back(std::move(t));
        }
    }
    result.lookedFor = static_cast<int>(wanted.size());
    if (wanted.empty())
        return result;

    // Index the folder: file name -> files with that name.
    std::unordered_map<std::string, std::vector<std::filesystem::path>> byName;
    std::set<std::string> wantedNames;
    for (const auto& t : wanted)
        wantedNames.insert(lowerName(fromUtf8(t.path)));
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied,
                                                     ec);
    for (const std::filesystem::recursive_directory_iterator end; !ec && it != end; it.increment(ec))
    {
        if (isCancelled && (result.filesIndexed % 512) == 0 && isCancelled())
        {
            result.cancelled = true;
            return result;
        }
        std::error_code typeError;
        if (!it->is_regular_file(typeError))
            continue;
        ++result.filesIndexed;
        if (const auto name = lowerName(it->path()); wantedNames.count(name) > 0)
            byName[name].push_back(it->path());
    }

    // Match: same name and size; several -> the same content hash.
    std::vector<std::pair<std::int64_t, std::string>> moves;
    std::set<std::string> claimed;
    for (const auto& t : wanted)
    {
        const auto found = byName.find(lowerName(fromUtf8(t.path)));
        std::vector<std::filesystem::path> sameSize;
        if (found != byName.end())
            for (const auto& candidate : found->second)
            {
                std::error_code sizeError;
                const auto size = std::filesystem::file_size(candidate, sizeError);
                if (!sizeError && (t.size < 0 || static_cast<std::int64_t>(size) == t.size))
                    sameSize.push_back(candidate);
            }
        std::optional<std::filesystem::path> pick;
        if (sameSize.size() == 1)
            pick = sameSize.front();
        else if (sameSize.size() > 1)
        {
            for (const auto& candidate : sameSize)
                if (const auto h = hashFileContent(candidate); h && h.value().hex == t.hash)
                {
                    pick = candidate;
                    break;
                }
            if (!pick)
                note("several files could be " + t.path + "; none has the same content");
        }
        if (!pick)
        {
            ++result.notFound;
            continue;
        }
        auto newPath = libraryPath(*pick);
        auto key = newPath;
        for (auto& c : key)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        if (takenPaths.count(key) > 0 || !claimed.insert(key).second)
        {
            note(newPath + " is already another track in the library");
            ++result.notFound;
            continue;
        }
        moves.emplace_back(t.id, std::move(newPath));
    }

    if (!moves.empty())
        db.writeAndWait(
            [&](Connection& c)
            {
                const bool ok = c.transaction(
                    [&]
                    {
                        auto* s = c.prepare("UPDATE tracks SET path = ?, missing = 0 WHERE id = ?");
                        if (s == nullptr)
                            return false;
                        for (const auto& [id, path] : moves)
                        {
                            if (!s->bind(1, path).bind(2, id).run())
                                return false;
                            c.noteChange(Table::Tracks, id);
                        }
                        return true;
                    });
                if (ok)
                    result.relocated = static_cast<int>(moves.size());
                else
                    note("couldn't save the new places: " + c.lastError());
            });
    return result;
}

} // namespace devildj::library
