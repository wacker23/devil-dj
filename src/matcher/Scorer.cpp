#include "matcher/Scorer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace devildj::matcher
{

namespace
{
std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

Weights weightsOf(std::initializer_list<double> values)
{
    Weights w;
    std::size_t i = 0;
    for (const double v : values)
        w.w[i++] = v;
    return w;
}
} // namespace

const char* featureName(FeatureId id) noexcept
{
    static const char* const names[] = {"tempo", "key", "energy", "sound", "groove", "vocal",
                                        "mix",   "tags", "history", "rating", "fresh"};
    const auto i = static_cast<int>(id);
    return i >= 0 && i < kFeatureCount ? names[i] : "";
}

std::vector<Preset> defaultPresets()
{
    //                                tempo key  energy sound groove vocal mix  tags hist rating fresh
    return {{"Balanced", weightsOf({1.0, 1.0, 1.0, 0.8, 0.6, 0.6, 0.4, 0.4, 0.5, 0.3, 0.2})},
            {"Harmonic", weightsOf({0.8, 2.0, 0.8, 0.6, 0.5, 0.8, 0.4, 0.3, 0.4, 0.3, 0.2})},
            {"Peak time", weightsOf({1.0, 0.8, 1.6, 0.8, 0.8, 0.6, 0.6, 0.3, 0.4, 0.4, 0.2})},
            {"Warm-up", weightsOf({1.0, 1.0, 1.2, 1.0, 0.6, 0.6, 0.6, 0.5, 0.4, 0.3, 0.3})},
            {"Open format", weightsOf({0.6, 0.6, 1.0, 0.4, 0.4, 0.8, 0.4, 0.8, 0.8, 0.4, 0.3})},
            {"Discovery", weightsOf({1.0, 1.0, 0.8, 0.8, 0.6, 0.6, 0.4, 0.3, 0.0, 0.0, 1.0})}};
}

std::string presetsToJson(const std::vector<Preset>& presets)
{
    std::ostringstream out;
    out << "[\n";
    for (std::size_t p = 0; p < presets.size(); ++p)
    {
        out << "  {\"name\": \"" << presets[p].name << "\", \"weights\": {";
        for (int i = 0; i < kFeatureCount; ++i)
            out << (i > 0 ? ", " : "") << "\"" << featureName(static_cast<FeatureId>(i))
                << "\": " << presets[p].weights.w[static_cast<std::size_t>(i)];
        out << "}}" << (p + 1 < presets.size() ? "," : "") << "\n";
    }
    out << "]\n";
    return out.str();
}

std::vector<Preset> presetsFromJson(const std::string& json)
{
    // [{"name": "...", "weights": {...}}, ...] - read by hand (a fixed, flat shape).
    std::vector<Preset> out;
    const auto balanced = defaultPresets().front().weights;
    std::size_t at = 0;
    for (;;)
    {
        const auto nameKey = json.find("\"name\"", at);
        if (nameKey == std::string::npos)
            break;
        const auto q1 = json.find('"', json.find(':', nameKey) + 1);
        const auto q2 = q1 == std::string::npos ? std::string::npos : json.find('"', q1 + 1);
        const auto open = json.find('{', q2 == std::string::npos ? nameKey : q2);
        const auto close = open == std::string::npos ? std::string::npos : json.find('}', open);
        if (q1 == std::string::npos || q2 == std::string::npos || close == std::string::npos)
            break;
        Preset p;
        p.name = json.substr(q1 + 1, q2 - q1 - 1);
        p.weights = balanced;
        const std::string body = json.substr(open, close - open + 1);
        for (int i = 0; i < kFeatureCount; ++i)
        {
            const std::string key = std::string("\"") + featureName(static_cast<FeatureId>(i)) + "\"";
            const auto k = body.find(key);
            if (k == std::string::npos)
                continue;
            const auto colon = body.find(':', k + key.size());
            if (colon == std::string::npos)
                continue;
            char* end = nullptr;
            const double v = std::strtod(body.c_str() + colon + 1, &end);
            if (end != body.c_str() + colon + 1 && v >= 0.0 && v <= 10.0)
                p.weights.w[static_cast<std::size_t>(i)] = v;
        }
        if (!p.name.empty())
            out.push_back(std::move(p));
        at = close + 1;
    }
    return out.empty() ? defaultPresets() : out;
}

const Preset* findPreset(const std::vector<Preset>& presets, const std::string& name)
{
    for (const auto& p : presets)
        if (p.name == name)
            return &p;
    return presets.empty() ? nullptr : &presets.front();
}

bool passesFilters(const Context& c, const TrackInfo& b)
{
    if (b.id == c.a.id || (!b.contentHash.empty() && b.contentHash == c.a.contentHash))
        return false;
    if (c.playedPolicy == PlayedPolicy::Exclude && c.playedThisSession.count(b.id) > 0)
        return false;
    if (b.rating < c.minRating)
        return false;
    if (!tempoReachable(c.aBpmNow, b.bpm, c.maxStretch))
        return false;
    if (c.keyPolicy != KeyPolicy::Off)
    {
        double stretch = 0.0;
        tempoFit(c.aBpmNow, b.bpm, &stretch);
        const auto key = keyFit(c.a.key, b.key, c.harmonic, stretch, c.keyLockOff, c.allowKeyShift);
        const double need = c.keyPolicy == KeyPolicy::Strict ? 0.85 : 0.5;
        if (!key.value || *key.value < need - 1e-9)
            return false;
    }
    return true;
}

Scored score(const Context& c, const TrackInfo& b, const Weights& weights)
{
    Scored s;
    s.track = &b;
    auto& f = s.features;
    double stretch = 0.0;
    f[static_cast<std::size_t>(FeatureId::Tempo)] = tempoFit(c.aBpmNow, b.bpm, &stretch);
    f[static_cast<std::size_t>(FeatureId::Key)] = keyFit(c.a.key, b.key, c.harmonic, stretch, c.keyLockOff, c.allowKeyShift);
    f[static_cast<std::size_t>(FeatureId::Energy)] =
        energyFit(c.a.energyOutro ? c.a.energyOutro : c.a.energy, b.energyIntro ? b.energyIntro : b.energy, c.intent);
    f[static_cast<std::size_t>(FeatureId::Sound)] = soundFit(c.a.sound, b.sound, c.c10, c.c90, c.intent);
    f[static_cast<std::size_t>(FeatureId::Groove)] = grooveFit(c.a.groove, b.groove);
    // Vocal and Mix: Phase 6 (no data: skipped).
    f[static_cast<std::size_t>(FeatureId::Tags)] = tagsFit(c.a.tags, b.tags);
    {
        const auto pair = c.pairCounts.find(b.id);
        const auto artists = c.artistPairCounts.find(lower(b.artist));
        f[static_cast<std::size_t>(FeatureId::History)] =
            historyFit(pair != c.pairCounts.end() ? pair->second : 0,
                       artists != c.artistPairCounts.end() ? artists->second : 0, 0, c.maxPairCount);
    }
    f[static_cast<std::size_t>(FeatureId::Rating)] = ratingFit(b.rating);
    f[static_cast<std::size_t>(FeatureId::Fresh)] = freshnessFit(b.playsLast30Days, c.maxPlaysLast30Days);

    // Weighted mean over the features that have data; intent switch: tags count 0, sound half.
    double sum = 0.0, total = 0.0;
    std::vector<std::pair<double, std::string>> contributions;
    for (int i = 0; i < kFeatureCount; ++i)
    {
        const auto id = static_cast<FeatureId>(i);
        const auto& feature = f[static_cast<std::size_t>(i)];
        if (!feature.value)
            continue;
        double w = weights[id];
        if (c.intent == Intent::Switch && id == FeatureId::Tags)
            w = 0.0;
        if (c.intent == Intent::Switch && id == FeatureId::Sound)
            w *= 0.5;
        sum += w * *feature.value;
        total += w;
        if (w > 0.0 && !feature.reason.empty())
            contributions.emplace_back(w * *feature.value, feature.reason);
    }
    s.score = total > 0.0 ? sum / total : 0.0;
    // Penalties (§4.11).
    if (!b.artist.empty())
        for (const auto& artist : c.lastArtists)
            if (lower(artist) == lower(b.artist))
            {
                s.penalty += 0.15;
                s.reasons.push_back("Same artist as just played");
                break;
            }
    if (c.playedPolicy == PlayedPolicy::Penalize && c.playedThisSession.count(b.id) > 0)
    {
        s.penalty += 0.30;
        s.reasons.push_back("Played earlier tonight");
    }
    s.score -= s.penalty;
    std::stable_sort(contributions.begin(), contributions.end(),
                     [](const auto& x, const auto& y) { return x.first > y.first; });
    for (const auto& [_, reason] : contributions)
    {
        if (s.reasons.size() >= 4)
            break;
        s.reasons.push_back(reason);
    }
    return s;
}

std::vector<Scored> rank(const Context& c, const std::vector<TrackInfo>& candidates, const Weights& weights, int count)
{
    std::vector<Scored> all;
    all.reserve(candidates.size());
    for (const auto& b : candidates)
        if (passesFilters(c, b))
            all.push_back(score(c, b, weights));
    std::stable_sort(all.begin(), all.end(), [](const Scored& x, const Scored& y) { return x.score > y.score; });
    if (all.empty() || count <= 0)
        return {};

    // MMR over the top 50 for all but the last slot: lambda x score - (1 - lambda) x the most similar already chosen;
    // a near-duplicate (sound cosine > 0.98) of one already chosen only while nothing else is left.
    constexpr double lambda = 0.8;
    const std::size_t pool = std::min<std::size_t>(50, all.size());
    const auto similar = [](const Scored& x, const Scored& y) { return cosine(x.track->sound, y.track->sound); };
    std::vector<Scored> chosen;
    std::vector<bool> used(all.size(), false);
    const std::size_t mmrSlots = std::min<std::size_t>(static_cast<std::size_t>(std::max(1, count - 1)), pool);
    while (chosen.size() < mmrSlots)
    {
        int best = -1;
        double bestValue = -1e9;
        bool bestNear = true;
        for (std::size_t i = 0; i < pool; ++i)
        {
            if (used[i])
                continue;
            double maxSim = 0.0;
            for (const auto& s : chosen)
                maxSim = std::max(maxSim, similar(all[i], s));
            const bool near = maxSim > 0.98;
            const double value = lambda * all[i].score - (1.0 - lambda) * maxSim;
            // A candidate that isn't a near-duplicate always beats one that is.
            if ((bestNear && !near) || (near == bestNear && value > bestValue))
            {
                best = static_cast<int>(i);
                bestValue = value;
                bestNear = near;
            }
        }
        if (best < 0)
            break;
        used[static_cast<std::size_t>(best)] = true;
        chosen.push_back(all[static_cast<std::size_t>(best)]);
    }
    // The wildcard: among those not chosen, the ones whose sound is least like A's (the lowest quarter), the best
    // scoring of them.
    if (static_cast<int>(chosen.size()) < count)
    {
        std::vector<std::pair<double, std::size_t>> unlike;
        const auto nearChosen = [&](std::size_t i)
        {
            for (const auto& s : chosen)
                if (similar(all[i], s) > 0.98)
                    return true;
            return false;
        };
        for (int pass = 0; pass < 2 && unlike.empty(); ++pass) // without near-duplicates of the chosen, if possible
            for (std::size_t i = 0; i < all.size(); ++i)
                if (!used[i] && (pass == 1 || !nearChosen(i)))
                    unlike.emplace_back(cosine(c.a.sound, all[i].track->sound), i);
        if (!unlike.empty())
        {
            std::sort(unlike.begin(), unlike.end());
            const std::size_t quarter = std::max<std::size_t>(1, unlike.size() / 4);
            std::size_t pick = unlike.front().second;
            for (std::size_t k = 0; k < quarter; ++k)
                if (all[unlike[k].second].score > all[pick].score)
                    pick = unlike[k].second;
            auto w = all[pick];
            w.wildcard = true;
            chosen.push_back(std::move(w));
        }
    }
    return chosen;
}

std::pair<double, double> soundPercentiles(const std::vector<TrackInfo>& tracks, int samplePairs)
{
    std::vector<const TrackInfo*> with;
    for (const auto& t : tracks)
        if (!t.sound.empty())
            with.push_back(&t);
    if (with.size() < 3)
        return {0.0, 1.0};
    std::vector<double> sims;
    const std::size_t n = with.size();
    for (int k = 0; k < samplePairs; ++k)
    {
        const std::size_t i = static_cast<std::size_t>(k) % n;
        const std::size_t j = (i + 1 + (static_cast<std::size_t>(k) * 7919u) % (n - 1)) % n;
        if (i != j)
            sims.push_back(cosine(with[i]->sound, with[j]->sound));
    }
    std::sort(sims.begin(), sims.end());
    const auto at = [&](double q) { return sims[static_cast<std::size_t>(std::clamp(q * (sims.size() - 1), 0.0, sims.size() - 1.0))]; };
    return {at(0.10), at(0.90)};
}

} // namespace devildj::matcher
