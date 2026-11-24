#include "app/PhraseEdit.h"

#include <algorithm>
#include <cmath>

namespace devildj::app
{

namespace
{
constexpr double kSame = 1e-3; // seconds: two starts this close are one

bool startsAt(const Phrases& phrases, double seconds)
{
    return std::any_of(phrases.begin(), phrases.end(),
                       [seconds](const auto& p) { return std::abs(p.startSeconds - seconds) < kSame; });
}

void sortByStart(Phrases& phrases)
{
    std::sort(phrases.begin(), phrases.end(), [](const auto& a, const auto& b) { return a.startSeconds < b.startSeconds; });
}
} // namespace

double nearestBarSeconds(const util::Beatgrid& grid, double sample, double rate)
{
    if (grid.empty() || rate <= 0.0)
        return sample / std::max(1.0, rate);
    const auto beat = grid.nearestBeat(sample);
    long long downbeat = beat;
    for (int k = 0; k < util::Beatgrid::kBeatsPerBar && grid.barBeatOf(downbeat) != 1; ++k)
        --downbeat;
    if (beat - downbeat >= util::Beatgrid::kBeatsPerBar / 2) // nearer the next bar
        downbeat += util::Beatgrid::kBeatsPerBar;
    return grid.sampleAt(static_cast<double>(downbeat)) / rate;
}

int nearestPhrase(const Phrases& phrases, double seconds, double maxGap)
{
    int best = -1;
    double gap = maxGap;
    for (std::size_t i = 0; i < phrases.size(); ++i)
        if (const double x = std::abs(phrases[i].startSeconds - seconds); x <= gap)
        {
            gap = x;
            best = static_cast<int>(i);
        }
    return best;
}

Phrases addPhrase(Phrases phrases, double seconds)
{
    if (startsAt(phrases, seconds))
        return phrases;
    sortByStart(phrases);
    double energy = phrases.empty() ? 0.5 : phrases.front().energy;
    for (const auto& p : phrases)
        if (p.startSeconds <= seconds)
            energy = p.energy;
    phrases.push_back({seconds, 0, energy});
    sortByStart(phrases);
    return phrases;
}

Phrases movePhrase(Phrases phrases, int index, double seconds)
{
    if (index < 0 || index >= static_cast<int>(phrases.size()) || startsAt(phrases, seconds))
        return phrases;
    phrases[static_cast<std::size_t>(index)].startSeconds = seconds;
    sortByStart(phrases);
    return phrases;
}

Phrases removePhrase(Phrases phrases, int index)
{
    if (index < 0 || index >= static_cast<int>(phrases.size()))
        return phrases;
    const auto i = static_cast<std::size_t>(index);
    if (i > 0)
    {
        auto& into = phrases[i - 1];
        const int total = std::max(1, into.bars + phrases[i].bars);
        into.energy = (into.energy * into.bars + phrases[i].energy * phrases[i].bars) / total;
    }
    phrases.erase(phrases.begin() + index);
    return phrases;
}

Phrases withBars(Phrases phrases, const util::Beatgrid& grid, double rate, double endSample)
{
    sortByStart(phrases);
    if (grid.empty() || rate <= 0.0)
        return phrases;
    for (std::size_t i = 0; i < phrases.size(); ++i)
    {
        const double a = phrases[i].startSeconds * rate;
        const double b = i + 1 < phrases.size() ? phrases[i + 1].startSeconds * rate : endSample;
        const double beats = grid.beatAt(b) - grid.beatAt(a);
        phrases[i].bars = std::max(1, static_cast<int>(std::lround(beats / util::Beatgrid::kBeatsPerBar)));
    }
    return phrases;
}

} // namespace devildj::app
