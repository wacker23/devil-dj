#include "PlaylistRepository.h"

#include "library/Connection.h"
#include "library/RepositoryUtil.h"

#include <algorithm>
#include <set>

namespace devildj::library
{

namespace
{
constexpr const char* kColumns = "id, parent_id, name, kind, rule_json, sort_order, created_at";

Playlist readPlaylist(const Statement& s)
{
    Playlist p;
    p.id = s.getInt(0);
    p.parentId = s.getOptionalInt(1);
    p.name = s.getText(2);
    p.kind = static_cast<PlaylistKind>(s.getInt(3));
    p.ruleJson = s.getText(4);
    p.sortOrder = static_cast<int>(s.getInt(5));
    p.createdAt = s.getInt(6);
    return p;
}

bool validKind(PlaylistKind k)
{
    return k == PlaylistKind::Folder || k == PlaylistKind::Playlist || k == PlaylistKind::Smart ||
           k == PlaylistKind::History;
}
} // namespace

PlaylistRepository::PlaylistRepository(Connection& connection) noexcept
    : db(connection)
{
}

bool PlaylistRepository::fail(std::string message)
{
    lastError = std::move(message);
    return false;
}

util::Result<std::int64_t> PlaylistRepository::create(const Playlist& playlist)
{
    using R = util::Result<std::int64_t>;
    const auto name = trimmed(playlist.name);
    if (name.empty())
        return fail("a playlist needs a name"), R::fail(lastError);
    if (!validKind(playlist.kind))
        return fail("unknown playlist kind"), R::fail(lastError);
    if (playlist.parentId)
    {
        const auto parent = get(*playlist.parentId);
        if (!parent || parent->kind != PlaylistKind::Folder)
            return fail("playlists can only go inside a folder"), R::fail(lastError);
    }
    int sortOrder = playlist.sortOrder;
    if (sortOrder == 0)
    {
        auto* s = db.prepare("SELECT coalesce(max(sort_order), 0) + 1 FROM playlists WHERE parent_id IS ?");
        if (s != nullptr && s->bind(1, playlist.parentId).step())
            sortOrder = static_cast<int>(s->getInt(0));
        if (s != nullptr)
            s->reset();
    }
    auto* s = db.prepare(
        "INSERT INTO playlists (parent_id, name, kind, rule_json, sort_order, created_at) VALUES (?, ?, ?, ?, ?, ?)");
    if (s == nullptr)
        return fail(db.lastError()), R::fail(lastError);
    s->bind(1, playlist.parentId).bind(2, name).bind(3, static_cast<int>(playlist.kind));
    s->bind(4, textOrNull(playlist.ruleJson)).bind(5, sortOrder);
    s->bind(6, playlist.createdAt > 0 ? playlist.createdAt : nowSeconds());
    if (!s->run())
        return fail(s->error()), R::fail(lastError);
    const auto id = db.lastInsertId();
    db.noteChange(Table::Playlists, id);
    lastError.clear();
    return R::ok(id);
}

std::optional<Playlist> PlaylistRepository::get(std::int64_t id)
{
    auto* s = db.prepare(std::string("SELECT ") + kColumns + " FROM playlists WHERE id = ?");
    if (s == nullptr || !s->bind(1, id).step())
        return std::nullopt;
    auto p = readPlaylist(*s);
    s->reset();
    return p;
}

std::vector<Playlist> PlaylistRepository::children(std::optional<std::int64_t> parentId)
{
    std::vector<Playlist> out;
    auto* s = db.prepare(std::string("SELECT ") + kColumns +
                         " FROM playlists WHERE parent_id IS ? ORDER BY sort_order, name COLLATE NOCASE, id");
    if (s == nullptr)
        return out;
    s->bind(1, parentId);
    while (s->step())
        out.push_back(readPlaylist(*s));
    return out;
}

std::vector<Playlist> PlaylistRepository::all()
{
    std::vector<Playlist> out;
    auto* s = db.prepare(std::string("SELECT ") + kColumns + " FROM playlists ORDER BY id");
    if (s == nullptr)
        return out;
    while (s->step())
        out.push_back(readPlaylist(*s));
    return out;
}

bool PlaylistRepository::isInside(std::int64_t folder, std::int64_t playlistId)
{
    // Walk up from `folder`; it is inside playlistId if we meet it.
    std::optional<std::int64_t> at = folder;
    std::set<std::int64_t> seen;
    while (at && seen.insert(*at).second)
    {
        if (*at == playlistId)
            return true;
        const auto p = get(*at);
        at = p ? p->parentId : std::nullopt;
    }
    return false;
}

bool PlaylistRepository::update(const Playlist& playlist)
{
    const auto existing = get(playlist.id);
    if (!existing)
        return fail("no playlist " + std::to_string(playlist.id));
    const auto name = trimmed(playlist.name);
    if (name.empty())
        return fail("a playlist needs a name");
    if (playlist.kind != existing->kind)
        return fail("a playlist's kind can't change");
    if (playlist.parentId)
    {
        const auto parent = get(*playlist.parentId);
        if (!parent || parent->kind != PlaylistKind::Folder)
            return fail("playlists can only go inside a folder");
        if (isInside(*playlist.parentId, playlist.id))
            return fail("a folder can't go inside itself");
    }
    auto* s = db.prepare("UPDATE playlists SET parent_id = ?, name = ?, rule_json = ?, sort_order = ?, "
                         "created_at = ? WHERE id = ?");
    if (s == nullptr)
        return fail(db.lastError());
    s->bind(1, playlist.parentId).bind(2, name).bind(3, textOrNull(playlist.ruleJson)).bind(4, playlist.sortOrder);
    s->bind(5, playlist.createdAt > 0 ? playlist.createdAt : existing->createdAt).bind(6, playlist.id);
    if (!s->run())
        return fail(s->error());
    db.noteChange(Table::Playlists, playlist.id);
    lastError.clear();
    return true;
}

bool PlaylistRepository::remove(std::int64_t id)
{
    if (!get(id))
        return fail("no playlist " + std::to_string(id));
    // Everything inside goes too (ON DELETE CASCADE); note each one for the listeners.
    std::vector<std::int64_t> gone{id};
    for (std::size_t i = 0; i < gone.size(); ++i)
        for (const auto& child : children(gone[i]))
            gone.push_back(child.id);
    auto* s = db.prepare("DELETE FROM playlists WHERE id = ?");
    if (s == nullptr)
        return fail(db.lastError());
    if (!s->bind(1, id).run())
        return fail(s->error());
    for (const auto g : gone)
        db.noteChange(Table::Playlists, g, true);
    lastError.clear();
    return true;
}

bool PlaylistRepository::holdsTracks(std::int64_t playlistId)
{
    const auto p = get(playlistId);
    if (!p)
        return fail("no playlist " + std::to_string(playlistId));
    if (p->kind != PlaylistKind::Playlist && p->kind != PlaylistKind::History)
        return fail(p->kind == PlaylistKind::Folder ? "folders hold playlists, not tracks"
                                                    : "a smart playlist's tracks come from its rules");
    return true;
}

std::vector<std::int64_t> PlaylistRepository::trackIds(std::int64_t playlistId)
{
    std::vector<std::int64_t> out;
    auto* s = db.prepare("SELECT track_id FROM playlist_tracks WHERE playlist_id = ? ORDER BY position");
    if (s == nullptr)
        return out;
    s->bind(1, playlistId);
    while (s->step())
        out.push_back(s->getInt(0));
    return out;
}

bool PlaylistRepository::setTracks(std::int64_t playlistId, std::span<const std::int64_t> ids)
{
    if (!holdsTracks(playlistId))
        return false;
    const bool ok = db.savepoint(
        [&]
        {
            auto* clear = db.prepare("DELETE FROM playlist_tracks WHERE playlist_id = ?");
            if (clear == nullptr)
                return false;
            if (!clear->bind(1, playlistId).run())
            {
                db.setError(clear->error());
                return false;
            }
            auto* insert = db.prepare("INSERT INTO playlist_tracks (playlist_id, track_id, position) VALUES (?, ?, ?)");
            if (insert == nullptr)
                return false;
            for (std::size_t i = 0; i < ids.size(); ++i)
                if (!insert->bind(1, playlistId).bind(2, ids[i]).bind(3, static_cast<std::int64_t>(i)).run())
                {
                    db.setError(insert->error());
                    return false;
                }
            return true;
        });
    if (!ok)
        return fail(db.lastError());
    db.noteChange(Table::PlaylistTracks, playlistId);
    lastError.clear();
    return true;
}

bool PlaylistRepository::addTracks(std::int64_t playlistId, std::span<const std::int64_t> ids, int insertAt)
{
    if (!holdsTracks(playlistId))
        return false;
    auto list = trackIds(playlistId);
    const auto at = insertAt < 0 || insertAt > static_cast<int>(list.size()) ? list.size()
                                                                              : static_cast<std::size_t>(insertAt);
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(at), ids.begin(), ids.end());
    return setTracks(playlistId, list);
}

bool PlaylistRepository::removeTracksAt(std::int64_t playlistId, std::span<const int> positions)
{
    if (!holdsTracks(playlistId))
        return false;
    auto list = trackIds(playlistId);
    std::vector<bool> drop(list.size(), false);
    for (const int p : positions)
    {
        if (p < 0 || p >= static_cast<int>(list.size()))
            return fail("position " + std::to_string(p) + " isn't in the playlist");
        drop[static_cast<std::size_t>(p)] = true;
    }
    std::vector<std::int64_t> kept;
    for (std::size_t i = 0; i < list.size(); ++i)
        if (!drop[i])
            kept.push_back(list[i]);
    return setTracks(playlistId, kept);
}

bool PlaylistRepository::moveTrack(std::int64_t playlistId, int from, int to)
{
    if (!holdsTracks(playlistId))
        return false;
    auto list = trackIds(playlistId);
    const int n = static_cast<int>(list.size());
    if (from < 0 || from >= n || to < 0 || to >= n)
        return fail("position isn't in the playlist");
    const auto id = list[static_cast<std::size_t>(from)];
    list.erase(list.begin() + from);
    list.insert(list.begin() + to, id);
    return setTracks(playlistId, list);
}

} // namespace devildj::library
