#include "CueRepository.h"

#include "library/Connection.h"
#include "library/RepositoryUtil.h"

namespace devildj::library
{

namespace
{
constexpr const char* kColumns = "id, track_id, kind, slot, start_sample, length_samples, label, color";

Cue readCue(const Statement& s)
{
    Cue c;
    c.id = s.getInt(0);
    c.trackId = s.getInt(1);
    c.kind = static_cast<CueKind>(s.getInt(2));
    c.slot = optionalInt(s, 3);
    c.startSample = s.getInt(4);
    c.lengthSamples = s.getInt(5);
    c.label = s.getText(6);
    if (const auto v = s.getOptionalInt(7))
        c.color = static_cast<std::uint32_t>(*v);
    return c;
}

std::optional<std::int64_t> colorValue(const Cue& c)
{
    if (c.color)
        return static_cast<std::int64_t>(*c.color);
    return std::nullopt;
}
} // namespace

CueRepository::CueRepository(Connection& connection) noexcept
    : db(connection)
{
}

bool CueRepository::fail(std::string message)
{
    lastError = std::move(message);
    return false;
}

std::string CueRepository::validate(const Cue& c)
{
    if (static_cast<int>(c.kind) < 0 || static_cast<int>(c.kind) > static_cast<int>(CueKind::MixOut))
        return "unknown cue kind";
    if (c.isHot() && (!c.slot || *c.slot < 0 || *c.slot >= kHotCueSlots))
        return "a hot cue needs a pad slot 0..15";
    if (!c.isHot() && c.slot)
        return "only hot cues have a pad slot";
    if (c.startSample < 0)
        return "a cue can't start before the track";
    if (c.isLoop() && c.lengthSamples <= 0)
        return "a loop needs a length";
    if (!c.isLoop() && c.lengthSamples != 0)
        return "only loops have a length";
    if (c.color && *c.color > 0xFFFFFFu)
        return "colour must be 0xRRGGBB";
    return {};
}

bool CueRepository::clearSlot(const Cue& c)
{
    if (!c.isHot())
        return true;
    auto* s = db.prepare("DELETE FROM cues WHERE track_id = ? AND kind IN (0, 3) AND slot = ? AND id != ?");
    if (s == nullptr)
        return fail(db.lastError());
    if (!s->bind(1, c.trackId).bind(2, c.slot).bind(3, c.id).run())
        return fail(s->error());
    return true;
}

util::Result<std::int64_t> CueRepository::add(const Cue& cue)
{
    using R = util::Result<std::int64_t>;
    if (auto why = validate(cue); !why.empty())
        return fail(why), R::fail(lastError);
    std::int64_t id = 0;
    const bool ok = db.savepoint(
        [&]
        {
            Cue fresh = cue;
            fresh.id = 0;
            if (!clearSlot(fresh))
                return false;
            auto* s = db.prepare("INSERT INTO cues (track_id, kind, slot, start_sample, length_samples, label, color) "
                                 "VALUES (?, ?, ?, ?, ?, ?, ?)");
            if (s == nullptr)
                return fail(db.lastError());
            s->bind(1, cue.trackId).bind(2, static_cast<int>(cue.kind)).bind(3, cue.slot).bind(4, cue.startSample);
            s->bind(5, cue.lengthSamples).bind(6, textOrNull(cue.label)).bind(7, colorValue(cue));
            if (!s->run())
                return fail(s->error());
            id = db.lastInsertId();
            return true;
        });
    if (!ok)
        return R::fail(lastError.empty() ? db.lastError() : lastError);
    db.noteChange(Table::Cues, cue.trackId);
    lastError.clear();
    return R::ok(id);
}

std::optional<Cue> CueRepository::get(std::int64_t id)
{
    auto* s = db.prepare(std::string("SELECT ") + kColumns + " FROM cues WHERE id = ?");
    if (s == nullptr || !s->bind(1, id).step())
        return std::nullopt;
    auto c = readCue(*s);
    s->reset();
    return c;
}

std::vector<Cue> CueRepository::forTrack(std::int64_t trackId)
{
    std::vector<Cue> out;
    auto* s = db.prepare(std::string("SELECT ") + kColumns +
                         " FROM cues WHERE track_id = ? ORDER BY slot IS NULL, slot, start_sample, id");
    if (s == nullptr)
        return out;
    s->bind(1, trackId);
    while (s->step())
        out.push_back(readCue(*s));
    return out;
}

std::optional<Cue> CueRepository::hotCue(std::int64_t trackId, int slot)
{
    auto* s = db.prepare(std::string("SELECT ") + kColumns +
                         " FROM cues WHERE track_id = ? AND kind IN (0, 3) AND slot = ?");
    if (s == nullptr || !s->bind(1, trackId).bind(2, slot).step())
        return std::nullopt;
    auto c = readCue(*s);
    s->reset();
    return c;
}

bool CueRepository::update(const Cue& cue)
{
    const auto existing = get(cue.id);
    if (!existing)
        return fail("no cue " + std::to_string(cue.id));
    if (existing->trackId != cue.trackId)
        return fail("a cue can't move to another track");
    if (auto why = validate(cue); !why.empty())
        return fail(why);
    const bool ok = db.savepoint(
        [&]
        {
            if (!clearSlot(cue))
                return false;
            auto* s = db.prepare("UPDATE cues SET kind = ?, slot = ?, start_sample = ?, length_samples = ?, "
                                 "label = ?, color = ? WHERE id = ?");
            if (s == nullptr)
                return fail(db.lastError());
            s->bind(1, static_cast<int>(cue.kind)).bind(2, cue.slot).bind(3, cue.startSample);
            s->bind(4, cue.lengthSamples).bind(5, textOrNull(cue.label)).bind(6, colorValue(cue)).bind(7, cue.id);
            return s->run() || fail(s->error());
        });
    if (!ok)
        return false;
    db.noteChange(Table::Cues, cue.trackId);
    lastError.clear();
    return true;
}

bool CueRepository::remove(std::int64_t id)
{
    const auto existing = get(id);
    if (!existing)
        return fail("no cue " + std::to_string(id));
    auto* s = db.prepare("DELETE FROM cues WHERE id = ?");
    if (s == nullptr)
        return fail(db.lastError());
    if (!s->bind(1, id).run())
        return fail(s->error());
    db.noteChange(Table::Cues, existing->trackId);
    lastError.clear();
    return true;
}

bool CueRepository::removeAllForTrack(std::int64_t trackId)
{
    auto* s = db.prepare("DELETE FROM cues WHERE track_id = ?");
    if (s == nullptr)
        return fail(db.lastError());
    if (!s->bind(1, trackId).run())
        return fail(s->error());
    db.noteChange(Table::Cues, trackId);
    lastError.clear();
    return true;
}

} // namespace devildj::library
