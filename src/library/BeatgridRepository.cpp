#include "BeatgridRepository.h"

#include "library/Connection.h"

namespace devildj::library
{

BeatgridRepository::BeatgridRepository(Connection& connection) noexcept
    : db(connection)
{
}

std::optional<StoredBeatgrid> BeatgridRepository::get(std::int64_t trackId)
{
    auto* s = db.prepare("SELECT g.anchors_json, g.locked, t.sample_rate FROM beatgrids g JOIN tracks t ON t.id = "
                         "g.track_id WHERE g.track_id = ?");
    if (s == nullptr || !s->bind(1, trackId).step())
        return std::nullopt;
    StoredBeatgrid g;
    g.anchorsJson = s->getText(0);
    g.locked = s->getInt(1) != 0;
    if (const auto r = s->getOptionalInt(2))
        g.sampleRate = static_cast<int>(*r);
    s->reset();
    return g;
}

bool BeatgridRepository::set(std::int64_t trackId, const std::string& json, bool locked, std::optional<double> bpm)
{
    const bool ok = db.savepoint(
        [&]
        {
            auto* g = db.prepare("INSERT INTO beatgrids (track_id, anchors_json, locked) VALUES (?, ?, ?) "
                                 "ON CONFLICT(track_id) DO UPDATE SET anchors_json = excluded.anchors_json, "
                                 "locked = excluded.locked");
            if (g == nullptr || !g->bind(1, trackId).bind(2, json).bind(3, locked ? 1 : 0).run())
            {
                lastError = g != nullptr ? g->error() : db.lastError();
                return false;
            }
            auto* t = db.prepare("UPDATE tracks SET bpm = coalesce(?, bpm), bpm_locked = ? WHERE id = ?");
            if (t == nullptr || !t->bind(1, bpm).bind(2, locked ? 1 : 0).bind(3, trackId).run())
            {
                lastError = t != nullptr ? t->error() : db.lastError();
                return false;
            }
            return true;
        });
    if (ok)
    {
        db.noteChange(Table::Tracks, trackId);
        lastError.clear();
    }
    return ok;
}

bool BeatgridRepository::remove(std::int64_t trackId)
{
    auto* s = db.prepare("DELETE FROM beatgrids WHERE track_id = ?");
    if (s == nullptr || !s->bind(1, trackId).run())
    {
        lastError = s != nullptr ? s->error() : db.lastError();
        return false;
    }
    db.noteChange(Table::Tracks, trackId);
    return true;
}

} // namespace devildj::library
