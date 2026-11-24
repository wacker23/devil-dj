#include "library/PhraseRepository.h"

#include "library/Connection.h"

#include <algorithm>

namespace devildj::library
{

PhraseRepository::PhraseRepository(Connection& connection) noexcept
    : db(connection)
{
}

StoredPhrases PhraseRepository::get(std::int64_t trackId)
{
    StoredPhrases out;
    auto* s = db.prepare("SELECT start_seconds, bars, energy, edited FROM phrases WHERE track_id = ? ORDER BY idx");
    if (s == nullptr)
        return out;
    s->bind(1, trackId);
    while (s->step())
    {
        out.phrases.push_back({s->getDouble(0), static_cast<int>(s->getInt(1)), s->getDouble(2)});
        out.edited = out.edited || s->getInt(3) != 0;
    }
    s->reset();
    return out;
}

bool PhraseRepository::edited(std::int64_t trackId)
{
    return db.queryInt("SELECT count(*) FROM phrases WHERE edited = 1 AND track_id = " + std::to_string(trackId))
               .value_or(0) > 0;
}

std::vector<std::int64_t> PhraseRepository::editedTracks()
{
    std::vector<std::int64_t> ids;
    auto* s = db.prepare("SELECT DISTINCT track_id FROM phrases WHERE edited = 1 ORDER BY track_id");
    if (s == nullptr)
        return ids;
    while (s->step())
        ids.push_back(s->getInt(0));
    s->reset();
    return ids;
}

bool PhraseRepository::write(std::int64_t trackId, const std::vector<StoredPhrase>& phrases, bool editedFlag)
{
    auto sorted = phrases;
    std::sort(sorted.begin(), sorted.end(),
              [](const StoredPhrase& a, const StoredPhrase& b) { return a.startSeconds < b.startSeconds; });
    auto* d = db.prepare("DELETE FROM phrases WHERE track_id = ?");
    if (d == nullptr || !d->bind(1, trackId).run())
    {
        lastError = d != nullptr ? d->error() : db.lastError();
        return false;
    }
    for (std::size_t i = 0; i < sorted.size(); ++i)
    {
        auto* s = db.prepare("INSERT INTO phrases (track_id, idx, start_seconds, bars, energy, edited) "
                             "VALUES (?, ?, ?, ?, ?, ?)");
        if (s == nullptr || !s->bind(1, trackId)
                                 .bind(2, static_cast<std::int64_t>(i))
                                 .bind(3, sorted[i].startSeconds)
                                 .bind(4, static_cast<std::int64_t>(sorted[i].bars))
                                 .bind(5, sorted[i].energy)
                                 .bind(6, editedFlag ? 1 : 0)
                                 .run())
        {
            lastError = s != nullptr ? s->error() : db.lastError();
            return false;
        }
    }
    return true;
}

bool PhraseRepository::set(std::int64_t trackId, const std::vector<StoredPhrase>& phrases, bool editedFlag)
{
    const bool ok = db.savepoint([&] { return write(trackId, phrases, editedFlag); });
    if (ok)
    {
        db.noteChange(Table::Tracks, trackId);
        lastError.clear();
    }
    return ok;
}

bool PhraseRepository::setFromAnalysis(std::int64_t trackId, const std::vector<StoredPhrase>& phrases)
{
    if (edited(trackId))
        return false; // the owner's phrases stay
    return set(trackId, phrases, false);
}

} // namespace devildj::library
