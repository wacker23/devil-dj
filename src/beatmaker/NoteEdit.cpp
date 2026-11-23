#include "beatmaker/NoteEdit.h"

#include <algorithm>
#include <cstdlib>

namespace devildj::beatmaker
{

const std::vector<Scale>& scales()
{
    static const std::vector<Scale> list{
        {"Chromatic", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
        {"Major", {0, 2, 4, 5, 7, 9, 11}},
        {"Minor", {0, 2, 3, 5, 7, 8, 10}},
        {"Dorian", {0, 2, 3, 5, 7, 9, 10}},
        {"Mixolydian", {0, 2, 4, 5, 7, 9, 10}},
        {"Harmonic minor", {0, 2, 3, 5, 7, 8, 11}},
        {"Pentatonic major", {0, 2, 4, 7, 9}},
        {"Pentatonic minor", {0, 3, 5, 7, 10}},
        {"Blues", {0, 3, 5, 6, 7, 10}},
    };
    return list;
}

bool inScale(int pitch, int root, int scale)
{
    const auto& s = scales()[static_cast<std::size_t>(std::clamp(scale, 0, static_cast<int>(scales().size()) - 1))];
    const int degree = ((pitch - root) % 12 + 12) % 12;
    return std::find(s.steps.begin(), s.steps.end(), degree) != s.steps.end();
}

int snapToScale(int pitch, int root, int scale)
{
    for (int d = 0; d < 12; ++d)
    {
        if (inScale(pitch - d, root, scale) && pitch - d >= 0)
            return pitch - d;
        if (inScale(pitch + d, root, scale) && pitch + d <= 127)
            return pitch + d;
    }
    return pitch;
}

int stepInScale(int pitch, int root, int scale, int direction)
{
    const int dir = direction >= 0 ? 1 : -1;
    for (int p = pitch + dir; p >= 0 && p <= 127; p += dir)
        if (inScale(p, root, scale))
            return p;
    return pitch;
}

const std::vector<Chord>& chords()
{
    static const std::vector<Chord> list{
        {"Note", {0}},          {"Major", {0, 4, 7}},     {"Minor", {0, 3, 7}},     {"7", {0, 4, 7, 10}},
        {"Maj7", {0, 4, 7, 11}}, {"Min7", {0, 3, 7, 10}}, {"Sus2", {0, 2, 7}},      {"Sus4", {0, 5, 7}},
    };
    return list;
}

std::vector<int> chordPitches(int root, int chord)
{
    std::vector<int> out;
    for (const int i : chords()[static_cast<std::size_t>(std::clamp(chord, 0, static_cast<int>(chords().size()) - 1))].intervals)
        if (root + i >= 0 && root + i <= 127)
            out.push_back(root + i);
    return out;
}

int snapTicks(int division, bool triplet)
{
    const int d = division <= 4 ? 4 : division <= 8 ? 8 : division <= 16 ? 16 : 32;
    const int ticks = 4 * kPpq / d; // 1/4 = 96 ... 1/32 = 12
    return triplet ? ticks * 2 / 3 : ticks;
}

int snapDown(int tick, int grid)
{
    grid = std::max(1, grid);
    return tick >= 0 ? tick / grid * grid : -((-tick + grid - 1) / grid * grid);
}

int snapNearest(int tick, int grid)
{
    grid = std::max(1, grid);
    return snapDown(tick + grid / 2, grid);
}

void sortNotes(SynthTrack& track, std::optional<std::size_t>* follow)
{
    std::vector<std::size_t> order(track.notes.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b)
                     { return track.notes[a].startTick < track.notes[b].startTick; });
    std::vector<Note> sorted;
    sorted.reserve(order.size());
    std::optional<std::size_t> moved;
    for (std::size_t i = 0; i < order.size(); ++i)
    {
        if (follow != nullptr && *follow && **follow == order[i])
            moved = i;
        sorted.push_back(track.notes[order[i]]);
    }
    track.notes = std::move(sorted);
    if (follow != nullptr)
        *follow = moved;
}

void quantize(SynthTrack& track, int grid, const std::vector<std::size_t>* only)
{
    for (std::size_t i = 0; i < track.notes.size(); ++i)
    {
        if (only != nullptr && std::find(only->begin(), only->end(), i) == only->end())
            continue;
        auto& n = track.notes[i];
        n.startTick = std::max(0, snapNearest(n.startTick, grid));
        n.lengthTicks = std::max(n.lengthTicks, grid);
    }
    sortNotes(track);
    // Duplicates (same start and pitch): keep the longer.
    std::vector<Note> kept;
    for (const auto& n : track.notes)
    {
        auto it = std::find_if(kept.begin(), kept.end(), [&](const Note& k) { return k.startTick == n.startTick && k.pitch == n.pitch; });
        if (it == kept.end())
            kept.push_back(n);
        else if (n.lengthTicks > it->lengthTicks)
            *it = n;
    }
    track.notes = std::move(kept);
}

std::optional<std::size_t> noteAt(const SynthTrack& track, int tick, int pitch)
{
    std::optional<std::size_t> found;
    for (std::size_t i = 0; i < track.notes.size(); ++i)
    {
        const auto& n = track.notes[i];
        if (n.pitch == pitch && tick >= n.startTick && tick < n.startTick + n.lengthTicks)
            found = i;
    }
    return found;
}

std::size_t addNotes(SynthTrack& track, const std::vector<Note>& notes)
{
    const std::size_t first = track.notes.size();
    for (const auto& n : notes)
        track.notes.push_back(n);
    std::optional<std::size_t> follow = notes.empty() ? std::nullopt : std::optional<std::size_t>(first);
    sortNotes(track, &follow);
    return follow.value_or(0);
}

} // namespace devildj::beatmaker
