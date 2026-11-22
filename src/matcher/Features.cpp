#include "matcher/Features.h"

#include "util/MusicalKey.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

namespace devildj::matcher
{

namespace
{
std::string format(const char* fmt, double v)
{
    char text[64];
    std::snprintf(text, sizeof(text), fmt, v);
    return text;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
} // namespace

// ---- tempo ----

static double bestStretch(double aBpmNow, double bBpm, double* multiple)
{
    double best = 1e9, bestM = 1.0;
    for (const double m : {0.5, 1.0, 2.0})
    {
        const double s = aBpmNow / (bBpm * m) - 1.0;
        if (std::abs(s) < std::abs(best))
        {
            best = s;
            bestM = m;
        }
    }
    if (multiple != nullptr)
        *multiple = bestM;
    return best;
}

bool tempoReachable(double aBpmNow, double bBpm, double maxStretch)
{
    if (!(aBpmNow > 0.0) || !(bBpm > 0.0))
        return false;
    return std::abs(bestStretch(aBpmNow, bBpm, nullptr)) <= maxStretch + 1e-12;
}

Feature tempoFit(double aBpmNow, double bBpm, double* stretch, double* multiple)
{
    Feature f;
    if (!(aBpmNow > 0.0) || !(bBpm > 0.0))
        return f;
    double m = 1.0;
    const double s = bestStretch(aBpmNow, bBpm, &m);
    if (stretch != nullptr)
        *stretch = s;
    if (multiple != nullptr)
        *multiple = m;
    f.value = std::exp(-std::pow(s / 0.03, 2.0));
    if (m != 1.0)
        f.reason = m == 2.0 ? "BPM x2" : "BPM /2"; // B at double / half time
    else
        f.reason = format("BPM %+.1f %%", 100.0 * s);
    return f;
}

// ---- key ----

Feature keyFit(int keyA, int keyB, const HarmonicTable& table, double stretch, bool keyLockOff, bool allowShift)
{
    Feature f;
    if (!util::isValidKey(keyA) || !util::isValidKey(keyB))
        return f;
    int effective = keyB;
    if (keyLockOff && stretch != 0.0) // the stretch moves B's pitch too
        effective = util::transposeKey(keyB, static_cast<int>(std::lround(12.0 * std::log2(1.0 + stretch))));
    double best = table.of(relation(keyA, effective));
    int shift = 0;
    if (allowShift)
        for (const int s : {-1, 1})
        {
            const double v = table.of(relation(keyA, util::transposeKey(effective, s))) - 0.1;
            if (v > best)
            {
                best = v;
                shift = s;
            }
        }
    f.value = best;
    f.reason = camelotCode(keyA) + " -> " + camelotCode(util::transposeKey(effective, shift));
    if (shift != 0)
        f.reason += shift > 0 ? " (shift +1)" : " (shift -1)";
    return f;
}

// ---- energy ----

int energyLevel(double energy) noexcept
{
    // 0..0.1 -> 1, ..., 0.9..1 -> 10 (a tenth each, its top included)
    return std::clamp(static_cast<int>(std::ceil(std::clamp(energy, 0.0, 1.0) * 10.0 - 1e-9)), 1, 10);
}

Feature energyFit(std::optional<double> a, std::optional<double> b, Intent intent)
{
    Feature f;
    if (!a || !b)
        return f;
    const double target = intent == Intent::Down ? -0.10 : intent == Intent::Up ? 0.10 : 0.0;
    const double tolerance = intent == Intent::Switch ? 0.25 : 0.15;
    f.value = std::exp(-std::pow((*b - *a - target) / tolerance, 2.0));
    const int la = energyLevel(*a), lb = energyLevel(*b);
    f.reason = "Energy " + std::to_string(la) + " -> " + std::to_string(lb) + (lb > la ? " up" : lb < la ? " down" : "");
    return f;
}

// ---- sound / groove ----

double cosine(std::span<const float> a, std::span<const float> b) noexcept
{
    if (a.empty() || a.size() != b.size())
        return 0.0;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        dot += static_cast<double>(a[i]) * b[i];
        na += static_cast<double>(a[i]) * a[i];
        nb += static_cast<double>(b[i]) * b[i];
    }
    return na > 0.0 && nb > 0.0 ? dot / std::sqrt(na * nb) : 0.0;
}

Feature soundFit(std::span<const float> a, std::span<const float> b, double c10, double c90, Intent intent)
{
    Feature f;
    if (a.empty() || b.empty() || a.size() != b.size())
        return f;
    const double c = cosine(a, b);
    const double span = c90 - c10;
    double v = span > 1e-9 ? std::clamp((c - c10) / span, 0.0, 1.0) : std::clamp(0.5 * (c + 1.0), 0.0, 1.0);
    f.reason = format("Similar sound %.0f %%", 100.0 * v);
    if (intent == Intent::Switch)
    {
        v = 1.0 - v; // contrast wanted
        f.reason = format("Contrast %.0f %%", 100.0 * v);
    }
    f.value = v;
    return f;
}

Feature grooveFit(std::span<const float> a, std::span<const float> b)
{
    Feature f;
    if (a.empty() || b.empty() || a.size() != b.size())
        return f;
    // Groove profiles are accent strengths (>= 0), so their cosine is already 0..1; anything below 0 counts as 0.
    const double v = std::clamp(cosine(a, b), 0.0, 1.0);
    f.value = v;
    f.reason = v >= 0.9 ? "Same groove" : v >= 0.7 ? "Similar groove" : "Different groove";
    return f;
}

// ---- tags ----

Feature tagsFit(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    Feature f;
    if (a.empty() || b.empty())
        return f;
    std::set<std::string> sa, sb;
    for (const auto& t : a)
        sa.insert(lower(t));
    for (const auto& t : b)
        sb.insert(lower(t));
    std::size_t common = 0;
    for (const auto& t : sa)
        common += sb.count(t);
    const std::size_t all = sa.size() + sb.size() - common;
    f.value = all > 0 ? static_cast<double>(common) / static_cast<double>(all) : 0.0;
    f.reason = common > 0 ? "Shared tags " + std::to_string(common) : "Other tags";
    return f;
}

// ---- history, rating, freshness ----

Feature historyFit(int nPair, int nArtistPair, int nTagPair, int nMax)
{
    Feature f;
    if (nMax <= 0)
    {
        f.value = 0.0;
        f.reason = "New pair";
        return f;
    }
    const auto g = [nMax](int n) { return std::log1p(std::max(0, n)) / std::log1p(nMax); };
    f.value = std::clamp(0.6 * g(nPair) + 0.25 * g(nArtistPair) + 0.15 * g(nTagPair), 0.0, 1.0);
    f.reason = nPair > 0 ? "You played this after A " + std::to_string(nPair) + "x" : "New pair";
    return f;
}

Feature ratingFit(int rating)
{
    Feature f;
    f.value = rating <= 0 ? 0.5 : std::clamp(rating, 0, 5) / 5.0;
    f.reason = rating <= 0 ? "Unrated" : std::to_string(rating) + " stars";
    return f;
}

Feature freshnessFit(int plays, int maxPlays)
{
    Feature f;
    f.value = maxPlays > 0 ? std::clamp(1.0 - static_cast<double>(std::max(0, plays)) / maxPlays, 0.0, 1.0) : 1.0;
    f.reason = plays > 0 ? "Played " + std::to_string(plays) + "x this month" : "Fresh";
    return f;
}

} // namespace devildj::matcher
