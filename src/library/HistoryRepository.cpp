#include "HistoryRepository.h"

#include "library/Connection.h"
#include "library/PlaylistRepository.h"

#include <ctime>
#include <fstream>

namespace devildj::library
{

namespace
{
std::tm localTime(std::int64_t unixSeconds)
{
    std::tm tm{};
    const auto t = static_cast<std::time_t>(unixSeconds);
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

std::string format(std::int64_t unixSeconds, const char* pattern)
{
    const auto tm = localTime(unixSeconds);
    char buf[64];
    return std::strftime(buf, sizeof(buf), pattern, &tm) > 0 ? std::string(buf) : std::string();
}
} // namespace

HistoryRepository::HistoryRepository(Connection& connection) noexcept
    : db(connection)
{
}

util::Result<std::int64_t> HistoryRepository::startSession(std::int64_t startedAt)
{
    PlaylistRepository playlists(db);
    Playlist p;
    p.name = format(startedAt, "%Y-%m-%d %H:%M");
    p.kind = PlaylistKind::History;
    p.createdAt = startedAt;
    auto id = playlists.create(p);
    if (!id)
        lastError = playlists.error();
    return id;
}

util::Result<std::int64_t> HistoryRepository::recordPlay(std::int64_t sessionId, std::int64_t trackId,
                                                         std::int64_t playedAt, int deck)
{
    using R = util::Result<std::int64_t>;
    std::int64_t id = 0;
    const bool ok = db.savepoint(
        [&]
        {
            PlaylistRepository playlists(db);
            const auto session = playlists.get(sessionId);
            if (!session || session->kind != PlaylistKind::History)
            {
                db.setError("no history session " + std::to_string(sessionId));
                return false;
            }
            // The transition: from the track played last in this session.
            std::optional<std::int64_t> previous;
            if (auto* s = db.prepare("SELECT track_id FROM history WHERE session_id = ? "
                                     "ORDER BY played_at DESC, id DESC LIMIT 1"))
            {
                if (s->bind(1, sessionId).step())
                    previous = s->getOptionalInt(0);
                s->reset();
            }
            const std::int64_t ids[] = {trackId};
            if (!playlists.addTracks(sessionId, ids))
            {
                db.setError(playlists.error());
                return false;
            }
            auto* s = db.prepare(
                "INSERT INTO history (session_id, track_id, played_at, deck, previous_track_id) VALUES (?, ?, ?, ?, ?)");
            if (s == nullptr)
                return false;
            s->bind(1, sessionId).bind(2, trackId).bind(3, playedAt).bind(4, deck).bind(5, previous);
            if (!s->run())
                return false;
            id = db.lastInsertId();
            db.noteChange(Table::Playlists, sessionId);
            return true;
        });
    if (!ok)
    {
        lastError = db.lastError();
        return R::fail(lastError);
    }
    return R::ok(id);
}

std::vector<HistoryEntry> HistoryRepository::entries(std::int64_t sessionId)
{
    std::vector<HistoryEntry> out;
    auto* s = db.prepare("SELECT id, session_id, track_id, played_at, deck, previous_track_id FROM history "
                         "WHERE session_id = ? ORDER BY played_at, id");
    if (s == nullptr)
        return out;
    s->bind(1, sessionId);
    while (s->step())
        out.push_back({s->getInt(0), s->getInt(1), s->getInt(2), s->getInt(3), static_cast<int>(s->getInt(4)),
                       s->getOptionalInt(5)});
    return out;
}

std::vector<std::int64_t> HistoryRepository::sessions()
{
    std::vector<std::int64_t> out;
    if (auto* s = db.prepare("SELECT id FROM playlists WHERE kind = ? ORDER BY created_at DESC, id DESC"))
    {
        s->bind(1, static_cast<int>(PlaylistKind::History));
        while (s->step())
            out.push_back(s->getInt(0));
    }
    return out;
}

std::vector<Transition> HistoryRepository::transitions()
{
    std::vector<Transition> out;
    if (auto* s = db.prepare("SELECT previous_track_id, track_id, played_at FROM history "
                             "WHERE previous_track_id IS NOT NULL ORDER BY played_at, id"))
        while (s->step())
            out.push_back({s->getInt(0), s->getInt(1), s->getInt(2)});
    return out;
}

int HistoryRepository::transitionCount(std::int64_t fromTrackId, std::int64_t toTrackId)
{
    auto* s = db.prepare("SELECT count(*) FROM history WHERE previous_track_id = ? AND track_id = ?");
    if (s == nullptr || !s->bind(1, fromTrackId).bind(2, toTrackId).step())
        return 0;
    const auto n = static_cast<int>(s->getInt(0));
    s->reset();
    return n;
}

std::vector<std::pair<std::int64_t, int>> HistoryRepository::playedAfter(std::int64_t fromTrackId)
{
    std::vector<std::pair<std::int64_t, int>> out;
    if (auto* s = db.prepare("SELECT track_id, count(*) AS n FROM history WHERE previous_track_id = ? "
                             "GROUP BY track_id ORDER BY n DESC, track_id"))
    {
        s->bind(1, fromTrackId);
        while (s->step())
            out.push_back({s->getInt(0), static_cast<int>(s->getInt(1))});
    }
    return out;
}

std::vector<std::string> HistoryRepository::tracklist(std::int64_t sessionId)
{
    std::vector<std::string> lines;
    auto* s = db.prepare("SELECT h.played_at, coalesce(t.artist, ''), coalesce(t.title, ''), coalesce(t.path, '') "
                         "FROM history h LEFT JOIN tracks t ON t.id = h.track_id WHERE h.session_id = ? "
                         "ORDER BY h.played_at, h.id");
    if (s == nullptr)
        return lines;
    s->bind(1, sessionId);
    int n = 0;
    while (s->step())
    {
        std::string artist = s->getText(1), title = s->getText(2);
        if (title.empty()) // untagged: the file name
        {
            const std::string path = s->getText(3);
            title = path.substr(path.find_last_of("\\/") == std::string::npos ? 0 : path.find_last_of("\\/") + 1);
        }
        char number[8];
        std::snprintf(number, sizeof(number), "%02d. ", ++n);
        lines.push_back(number + format(s->getInt(0), "%H:%M") + "  " + (artist.empty() ? "" : artist + " - ") +
                        title);
    }
    return lines;
}

util::Result<int> HistoryRepository::exportTracklist(std::int64_t sessionId, const std::filesystem::path& file)
{
    using R = util::Result<int>;
    PlaylistRepository playlists(db);
    const auto session = playlists.get(sessionId);
    if (!session || session->kind != PlaylistKind::History)
        return lastError = "no history session " + std::to_string(sessionId), R::fail(lastError);
    const auto lines = tracklist(sessionId);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out)
        return lastError = "can't write " + file.string(), R::fail(lastError);
    out << "\xEF\xBB\xBF" << session->name << "\r\n\r\n"; // a BOM, so Notepad shows the UTF-8 right
    for (const auto& line : lines)
        out << line << "\r\n";
    if (!out)
        return lastError = "can't write " + file.string(), R::fail(lastError);
    return R::ok(static_cast<int>(lines.size()));
}

} // namespace devildj::library
