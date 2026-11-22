#include "HotCueStore.h"

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
} // namespace

std::uint32_t defaultHotCueColour(int slot) noexcept
{
    // The track colour palette (ui/TrackColours.h), in its order.
    static constexpr std::uint32_t colours[8] = {0xE0483C, 0xF08A24, 0xE8C832, 0x4CBF5A,
                                                 0x38C6C0, 0x3C7CE0, 0x9A5CE0, 0xE05CAE};
    return colours[((slot % 8) + 8) % 8];
}

char hotCueLetter(int slot) noexcept
{
    return static_cast<char>('A' + std::clamp(slot, 0, engine::kHotCues - 1));
}

HotCues loadHotCues(library::Connection& db, std::int64_t trackId, double fileRate, double bufferRate)
{
    HotCues out;
    if (trackId == 0)
        return out;
    const double k = scale(fileRate, bufferRate);
    for (const auto& c : library::CueRepository(db).forTrack(trackId))
    {
        if (!c.isHot() || !c.slot || *c.slot < 0 || *c.slot >= engine::kHotCues)
            continue;
        auto& h = out[static_cast<std::size_t>(*c.slot)];
        h.set = true;
        h.start = static_cast<double>(c.startSample) * k;
        h.length = static_cast<double>(c.lengthSamples) * k;
        h.colour = c.color.value_or(defaultHotCueColour(*c.slot));
        h.label = c.label;
    }
    return out;
}

engine::SetHotCues toEngine(engine::DeckId deck, const HotCues& cues) noexcept
{
    engine::SetHotCues s{deck, {}};
    for (std::size_t i = 0; i < cues.size(); ++i)
        s.cues[i] = {cues[i].set, cues[i].start, cues[i].length};
    return s;
}

HotCueChange changeFor(const HotCues& known, int slot, double fileRate, double bufferRate)
{
    const double k = scale(fileRate, bufferRate);
    const auto& h = known[static_cast<std::size_t>(slot)];
    HotCueChange c;
    c.slot = slot;
    c.set = h.set;
    c.start = static_cast<std::int64_t>(std::llround(h.start / k));
    c.length = static_cast<std::int64_t>(std::llround(h.length / k));
    c.colour = h.colour;
    c.label = h.label;
    return c;
}

std::vector<HotCueChange> diffHotCues(HotCues& known, const std::array<double, engine::kHotCues>& starts,
                                      const std::array<double, engine::kHotCues>& lengths, double fileRate,
                                      double bufferRate)
{
    std::vector<HotCueChange> out;
    for (int i = 0; i < engine::kHotCues; ++i)
    {
        auto& h = known[static_cast<std::size_t>(i)];
        const bool set = starts[static_cast<std::size_t>(i)] >= 0.0;
        if (!set && !h.set)
            continue;
        if (set && h.set && std::abs(h.start - starts[static_cast<std::size_t>(i)]) < 0.5 &&
            std::abs(h.length - lengths[static_cast<std::size_t>(i)]) < 0.5)
            continue;
        if (set)
        {
            if (!h.set) // new on this pad: its colour, no label
            {
                h.colour = defaultHotCueColour(i);
                h.label.clear();
            }
            h.set = true;
            h.start = starts[static_cast<std::size_t>(i)];
            h.length = lengths[static_cast<std::size_t>(i)];
        }
        else
        {
            h = {};
        }
        out.push_back(changeFor(known, i, fileRate, bufferRate));
    }
    return out;
}

bool applyHotCueChanges(library::Connection& db, std::int64_t trackId, const std::vector<HotCueChange>& changes)
{
    if (trackId == 0)
        return true;
    return db.savepoint(
        [&]
        {
            library::CueRepository repo(db);
            for (const auto& c : changes)
            {
                if (!c.set)
                {
                    if (const auto old = repo.hotCue(trackId, c.slot))
                        if (!repo.remove(old->id))
                            return false;
                    continue;
                }
                library::Cue cue;
                cue.trackId = trackId;
                cue.kind = c.length > 0 ? library::CueKind::HotLoop : library::CueKind::HotCue;
                cue.slot = c.slot;
                cue.startSample = c.start;
                cue.lengthSamples = c.length;
                cue.label = c.label;
                cue.color = c.colour;
                if (!repo.add(cue)) // replaces what was on the pad
                    return false;
            }
            return true;
        });
}

engine::HotCue hotCueForPad(engine::DeckId deck, int pad, int page, bool alternate, bool release) noexcept
{
    const auto slot = static_cast<std::uint8_t>(std::clamp(page, 0, 1) * 8 + std::clamp(pad, 0, 7));
    const auto op = alternate ? engine::HotCueOp::Delete : release ? engine::HotCueOp::Release : engine::HotCueOp::Press;
    return {deck, slot, op};
}

HotCues sortedByTime(const HotCues& cues)
{
    std::vector<HotCueInfo> set;
    for (const auto& c : cues)
        if (c.set)
            set.push_back(c);
    std::stable_sort(set.begin(), set.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
    HotCues out;
    for (std::size_t i = 0; i < set.size(); ++i)
        out[i] = set[i];
    return out;
}

} // namespace devildj::app
