#include "MemoryCues.h"

#include "library/Connection.h"
#include "library/CueRepository.h"

#include <algorithm>
#include <cmath>

namespace devildj::app
{

namespace
{
double scale(double fileRate, double bufferRate)
{
    return fileRate > 0.0 && bufferRate > 0.0 ? bufferRate / fileRate : 1.0;
}
constexpr double kSameSpotSeconds = 0.010;
} // namespace

std::vector<MemoryCue> loadMemoryCues(library::Connection& db, std::int64_t trackId, double fileRate, double bufferRate)
{
    std::vector<MemoryCue> out;
    if (trackId == 0)
        return out;
    const double k = scale(fileRate, bufferRate);
    for (const auto& c : library::CueRepository(db).forTrack(trackId))
        if (c.kind == library::CueKind::MemoryCue || c.kind == library::CueKind::MemoryLoop)
            out.push_back({c.id, c.kind == library::CueKind::MemoryLoop, static_cast<double>(c.startSample) * k,
                           static_cast<double>(c.lengthSamples) * k, c.color, c.label});
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    return out;
}

util::Result<std::int64_t> addMemoryCue(library::Connection& db, std::int64_t trackId, bool loop, double start,
                                        double length, double fileRate, double bufferRate, const std::string& label)
{
    using R = util::Result<std::int64_t>;
    if (trackId == 0)
        return R::fail("the track isn't in the library");
    if (loop && length <= 0.0)
        return R::fail("a memory loop needs a length");
    const double k = scale(fileRate, bufferRate);
    const auto existing = loadMemoryCues(db, trackId, fileRate, bufferRate);
    const auto ofKind = std::count_if(existing.begin(), existing.end(), [&](const auto& c) { return c.loop == loop; });
    if (ofKind >= (loop ? kMaxMemoryLoops : kMaxMemoryCues))
        return R::fail(loop ? "this track has 10 memory loops already" : "this track has 10 memory cues already");
    const double sameSpot = kSameSpotSeconds * (bufferRate > 0.0 ? bufferRate : 48000.0);
    for (const auto& c : existing)
        if (c.loop == loop && std::abs(c.start - start) < sameSpot)
            return R::fail("there is one here already");
    library::Cue cue;
    cue.trackId = trackId;
    cue.kind = loop ? library::CueKind::MemoryLoop : library::CueKind::MemoryCue;
    cue.startSample = static_cast<std::int64_t>(std::llround(std::max(0.0, start) / k));
    cue.lengthSamples = loop ? std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(length / k))) : 0;
    cue.label = label;
    library::CueRepository repo(db);
    auto id = repo.add(cue);
    if (!id)
        return R::fail(repo.error());
    return id;
}

bool renameMemoryCue(library::Connection& db, std::int64_t id, const std::string& label)
{
    library::CueRepository repo(db);
    auto c = repo.get(id);
    if (!c)
        return false;
    c->label = label;
    return repo.update(*c);
}

bool recolourMemoryCue(library::Connection& db, std::int64_t id, std::optional<std::uint32_t> colour)
{
    library::CueRepository repo(db);
    auto c = repo.get(id);
    if (!c)
        return false;
    c->color = colour;
    return repo.update(*c);
}

bool removeMemoryCue(library::Connection& db, std::int64_t id)
{
    return library::CueRepository(db).remove(id);
}

std::optional<MemoryCue> nextMemoryCue(const std::vector<MemoryCue>& cues, double position)
{
    for (const auto& c : cues) // by position
        if (c.start > position + 1.0)
            return c;
    return std::nullopt;
}

std::optional<MemoryCue> previousMemoryCue(const std::vector<MemoryCue>& cues, double position)
{
    for (auto it = cues.rbegin(); it != cues.rend(); ++it)
        if (it->start < position - 1.0)
            return *it;
    return std::nullopt;
}

} // namespace devildj::app
