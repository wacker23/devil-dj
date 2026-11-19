#include "TrackRepository.h"

#include "library/Connection.h"
#include "library/RepositoryUtil.h"

namespace devildj::library
{

namespace
{
constexpr const char* kColumns =
    "id, path, content_hash, title, artist, album, genre, year, comment, duration_ms, sample_rate, bitrate, "
    "file_size, bpm, bpm_locked, musical_key, key_confidence, loudness_lufs, auto_gain_db, rating, color, "
    "play_count, last_played, date_added, analysis_version, missing";

Track readTrack(const Statement& s)
{
    Track t;
    t.id = s.getInt(0);
    t.path = s.getText(1);
    t.contentHash = s.getText(2);
    t.title = s.getText(3);
    t.artist = s.getText(4);
    t.album = s.getText(5);
    t.genre = s.getText(6);
    t.year = optionalInt(s, 7);
    t.comment = s.getText(8);
    t.durationMs = s.getOptionalInt(9);
    t.sampleRate = optionalInt(s, 10);
    t.bitrateKbps = optionalInt(s, 11);
    t.fileSize = s.getOptionalInt(12);
    t.bpm = s.getOptionalDouble(13);
    t.bpmLocked = s.getInt(14) != 0;
    t.musicalKey = optionalInt(s, 15);
    t.keyConfidence = s.getOptionalDouble(16);
    t.loudnessLufs = s.getOptionalDouble(17);
    t.autoGainDb = s.getOptionalDouble(18);
    t.rating = static_cast<int>(s.getInt(19));
    if (const auto c = s.getOptionalInt(20))
        t.color = static_cast<std::uint32_t>(*c);
    t.playCount = static_cast<int>(s.getInt(21));
    t.lastPlayed = s.getOptionalInt(22);
    t.dateAdded = s.getInt(23);
    t.analysisVersion = static_cast<int>(s.getInt(24));
    t.missing = s.getInt(25) != 0;
    return t;
}

// Binds every column but id, starting at parameter 1 (25 parameters).
void bindTrack(Statement& s, const Track& t)
{
    std::optional<std::int64_t> color;
    if (t.color)
        color = static_cast<std::int64_t>(*t.color);
    s.bind(1, t.path).bind(2, t.contentHash).bind(3, textOrNull(t.title)).bind(4, textOrNull(t.artist));
    s.bind(5, textOrNull(t.album)).bind(6, textOrNull(t.genre)).bind(7, t.year).bind(8, textOrNull(t.comment));
    s.bind(9, t.durationMs).bind(10, t.sampleRate).bind(11, t.bitrateKbps).bind(12, t.fileSize).bind(13, t.bpm);
    s.bind(14, t.bpmLocked ? 1 : 0).bind(15, t.musicalKey).bind(16, t.keyConfidence).bind(17, t.loudnessLufs);
    s.bind(18, t.autoGainDb).bind(19, t.rating).bind(20, color).bind(21, t.playCount).bind(22, t.lastPlayed);
    s.bind(23, t.dateAdded > 0 ? t.dateAdded : nowSeconds()).bind(24, t.analysisVersion).bind(25, t.missing ? 1 : 0);
}

std::string validate(const Track& t)
{
    if (t.path.empty())
        return "a track needs a path";
    if (t.contentHash.empty())
        return "a track needs a content hash";
    if (t.rating < 0 || t.rating > 5)
        return "rating must be 0..5 stars";
    if (t.musicalKey && (*t.musicalKey < 0 || *t.musicalKey > 23))
        return "musical key must be 0..23";
    if (t.color && *t.color > 0xFFFFFFu)
        return "colour must be 0xRRGGBB";
    return {};
}
} // namespace

TrackRepository::TrackRepository(Connection& connection) noexcept
    : db(connection)
{
}

bool TrackRepository::fail(std::string message)
{
    lastError = std::move(message);
    return false;
}

util::Result<std::int64_t> TrackRepository::add(const Track& track)
{
    using R = util::Result<std::int64_t>;
    if (auto why = validate(track); !why.empty())
        return fail(why), R::fail(lastError);
    auto* s = db.prepare("INSERT INTO tracks (path, content_hash, title, artist, album, genre, year, comment, "
                         "duration_ms, sample_rate, bitrate, file_size, bpm, bpm_locked, musical_key, "
                         "key_confidence, loudness_lufs, auto_gain_db, rating, color, play_count, last_played, "
                         "date_added, analysis_version, missing) "
                         "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (s == nullptr)
        return fail(db.lastError()), R::fail(lastError);
    bindTrack(*s, track);
    if (!s->run())
        return fail(s->error()), R::fail(lastError);
    const auto id = db.lastInsertId();
    db.noteChange(Table::Tracks, id);
    lastError.clear();
    return R::ok(id);
}

std::optional<Track> TrackRepository::get(std::int64_t id)
{
    auto* s = db.prepare(std::string("SELECT ") + kColumns + " FROM tracks WHERE id = ?");
    if (s == nullptr || !s->bind(1, id).step())
        return std::nullopt;
    auto t = readTrack(*s);
    s->reset();
    return t;
}

std::optional<Track> TrackRepository::findByPath(const std::string& path)
{
    auto* s = db.prepare(std::string("SELECT ") + kColumns + " FROM tracks WHERE path = ?");
    if (s == nullptr || !s->bind(1, path).step())
        return std::nullopt;
    auto t = readTrack(*s);
    s->reset();
    return t;
}

std::vector<Track> TrackRepository::list(int offset, int limit)
{
    std::vector<Track> out;
    auto* s = db.prepare(std::string("SELECT ") + kColumns + " FROM tracks ORDER BY id LIMIT ? OFFSET ?");
    if (s == nullptr)
        return out;
    s->bind(1, limit).bind(2, offset);
    while (s->step())
        out.push_back(readTrack(*s));
    return out;
}

std::int64_t TrackRepository::count()
{
    return db.queryInt("SELECT count(*) FROM tracks").value_or(0);
}

bool TrackRepository::update(const Track& track)
{
    if (auto why = validate(track); !why.empty())
        return fail(why);
    auto* s = db.prepare("UPDATE tracks SET path = ?, content_hash = ?, title = ?, artist = ?, album = ?, genre = ?, "
                         "year = ?, comment = ?, duration_ms = ?, sample_rate = ?, bitrate = ?, file_size = ?, "
                         "bpm = ?, bpm_locked = ?, musical_key = ?, key_confidence = ?, loudness_lufs = ?, "
                         "auto_gain_db = ?, rating = ?, color = ?, play_count = ?, last_played = ?, date_added = ?, "
                         "analysis_version = ?, missing = ? WHERE id = ?");
    if (s == nullptr)
        return fail(db.lastError());
    bindTrack(*s, track);
    s->bind(26, track.id);
    if (!s->run())
        return fail(s->error());
    if (db.changes() == 0)
        return fail("no track " + std::to_string(track.id));
    db.noteChange(Table::Tracks, track.id);
    lastError.clear();
    return true;
}

bool TrackRepository::remove(std::int64_t id)
{
    std::vector<std::int64_t> playlists;
    {
        auto* s = db.prepare("SELECT DISTINCT playlist_id FROM playlist_tracks WHERE track_id = ?");
        if (s == nullptr)
            return fail(db.lastError());
        s->bind(1, id);
        while (s->step())
            playlists.push_back(s->getInt(0));
    }
    bool found = false;
    const bool ok = db.savepoint(
        [&]
        {
            // History keeps its rows (play counts, the Track Matcher's transitions) without the track.
            if (!db.exec("UPDATE history SET track_id = NULL WHERE track_id = " + std::to_string(id)) ||
                !db.exec("UPDATE history SET previous_track_id = NULL WHERE previous_track_id = " +
                         std::to_string(id)))
                return false;
            auto* s = db.prepare("DELETE FROM tracks WHERE id = ?");
            if (s == nullptr || !s->bind(1, id).run())
            {
                if (s != nullptr)
                    db.setError(s->error());
                return false;
            }
            found = db.changes() > 0;
            return true;
        });
    if (!ok)
        return fail(db.lastError());
    if (!found)
        return fail("no track " + std::to_string(id));
    db.noteChange(Table::Tracks, id, true);
    db.noteChange(Table::Cues, id);
    db.noteChange(Table::TrackTags, id);
    for (const auto p : playlists)
        db.noteChange(Table::PlaylistTracks, p);
    lastError.clear();
    return true;
}

} // namespace devildj::library
