#include "library/CueFeedbackRepository.h"

#include "library/Connection.h"

namespace devildj::library
{

CueFeedbackRepository::CueFeedbackRepository(Connection& connection) noexcept
    : db(connection)
{
}

bool CueFeedbackRepository::add(const CueAnswer& a)
{
    auto* s = db.prepare("INSERT INTO cue_feedback (track_id, start_seconds, reason, accepted, at) VALUES (?, ?, ?, ?, ?)");
    if (s == nullptr || !s->bind(1, a.trackId).bind(2, a.startSeconds).bind(3, a.reason).bind(4, a.accepted ? 1 : 0).bind(5, a.at).run())
    {
        lastError = s != nullptr ? s->error() : db.lastError();
        return false;
    }
    lastError.clear();
    return true;
}

std::vector<CueAnswer> CueFeedbackRepository::query(const std::string& where, std::int64_t trackId)
{
    std::vector<CueAnswer> out;
    auto* s = db.prepare("SELECT track_id, start_seconds, reason, accepted, at FROM cue_feedback " + where +
                         " ORDER BY id");
    if (s == nullptr)
        return out;
    if (!where.empty())
        s->bind(1, trackId);
    while (s->step())
        out.push_back({s->getInt(0), s->getDouble(1), s->getText(2), s->getInt(3) != 0, s->getInt(4)});
    s->reset();
    return out;
}

std::vector<CueAnswer> CueFeedbackRepository::forTrack(std::int64_t trackId)
{
    return query("WHERE track_id = ?", trackId);
}

std::vector<CueAnswer> CueFeedbackRepository::all()
{
    return query({}, 0);
}

int CueFeedbackRepository::count(bool accepted)
{
    return static_cast<int>(
        db.queryInt(std::string("SELECT count(*) FROM cue_feedback WHERE accepted = ") + (accepted ? "1" : "0")).value_or(0));
}

} // namespace devildj::library
