#include "matcher/Evaluation.h"

#include "library/Connection.h"
#include "matcher/CandidateQuery.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

namespace devildj::matcher
{

namespace
{
constexpr std::int64_t kMonth = 30 * 24 * 3600;

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string fixed(double v, int digits = 3)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.*f", digits, v);
    return buffer;
}
} // namespace

EvalData loadEvalData(library::Connection& db)
{
    EvalData d;
    d.tracks = loadCandidates(db, {}, 0, 0, 0); // every analyzed, present track (plays are recomputed per transition)
    for (std::size_t i = 0; i < d.tracks.size(); ++i)
        d.index[d.tracks[i].id] = i;
    if (auto* s = db.prepare("SELECT id, date_added FROM tracks"))
        while (s->step())
            d.dateAdded[s->getInt(0)] = s->getInt(1);
    if (auto* s = db.prepare("SELECT track_id, coalesce(previous_track_id, 0), played_at, session_id FROM history "
                             "ORDER BY played_at, id"))
        while (s->step())
        {
            const EvalData::Play p{s->getInt(0), s->getInt(1), s->getInt(2)};
            d.history.push_back(p);
            if (p.previous != 0 && p.previous != p.track)
                d.transitions.push_back({p.previous, p.track, p.at, s->getInt(3)});
        }
    const auto [c10, c90] = soundPercentiles(d.tracks);
    d.c10 = c10;
    d.c90 = c90;
    return d;
}

ReplayCase replayCase(const EvalData& data, const EvalTransition& t, KeyPolicy keyPolicy)
{
    ReplayCase out;
    const auto ia = data.index.find(t.a);
    const auto ib = data.index.find(t.b);
    if (ia == data.index.end() || ib == data.index.end())
        return out; // A or B isn't analyzed (any more): nothing to rank
    Context c;
    c.a = data.tracks[ia->second];
    c.aBpmNow = c.a.bpm;
    c.keyPolicy = keyPolicy;
    c.playedPolicy = PlayedPolicy::Ignore;
    c.c10 = data.c10;
    c.c90 = data.c90;
    // The history as it was before this transition: pairs after A, artist pairs, plays in the last 30 days.
    std::unordered_map<std::int64_t, int> plays;
    std::unordered_map<std::int64_t, std::unordered_map<std::int64_t, int>> pairCount; // previous -> track -> n
    const auto artistOf = [&](std::int64_t id) -> std::string
    {
        const auto it = data.index.find(id);
        return it != data.index.end() ? lower(data.tracks[it->second].artist) : std::string();
    };
    const auto aArtist = lower(c.a.artist);
    for (const auto& p : data.history)
    {
        if (p.at >= t.at)
            break;
        if (p.at >= t.at - kMonth)
            ++plays[p.track];
        if (p.previous == 0)
            continue;
        const int n = ++pairCount[p.previous][p.track];
        c.maxPairCount = std::max(c.maxPairCount, n);
        if (p.previous == t.a)
            c.pairCounts[p.track] = n;
        if (!aArtist.empty() && artistOf(p.previous) == aArtist)
            ++c.artistPairCounts[artistOf(p.track)];
    }
    for (const auto& [id, n] : plays)
        c.maxPlaysLast30Days = std::max(c.maxPlaysLast30Days, n);
    const auto weights = defaultPresets().front().weights; // features don't depend on them
    for (const auto& track : data.tracks)
    {
        if (track.id == t.a)
            continue;
        if (const auto added = data.dateAdded.find(track.id); added != data.dateAdded.end() && added->second > t.at)
            continue; // not in the library yet
        auto b = track;
        const auto pl = plays.find(b.id);
        b.playsLast30Days = pl != plays.end() ? pl->second : 0;
        if (!passesFilters(c, b))
            continue;
        const auto s = score(c, b, weights);
        std::array<std::optional<double>, kFeatureCount> f{};
        for (int i = 0; i < kFeatureCount; ++i)
            f[static_cast<std::size_t>(i)] = s.features[static_cast<std::size_t>(i)].value;
        if (b.id == t.b)
            out.b = static_cast<int>(out.features.size());
        out.features.push_back(f);
    }
    return out;
}

double weightedScore(const std::array<std::optional<double>, kFeatureCount>& f, const Weights& w) noexcept
{
    double sum = 0.0, total = 0.0;
    for (int i = 0; i < kFeatureCount; ++i)
        if (const auto& v = f[static_cast<std::size_t>(i)])
        {
            sum += w.w[static_cast<std::size_t>(i)] * *v;
            total += w.w[static_cast<std::size_t>(i)];
        }
    return total > 0.0 ? sum / total : 0.0;
}

int rankOfB(const ReplayCase& c, const Weights& w)
{
    if (c.b < 0)
        return 0;
    const double sb = weightedScore(c.features[static_cast<std::size_t>(c.b)], w);
    int better = 0, tied = 0;
    for (std::size_t i = 0; i < c.features.size(); ++i)
    {
        if (static_cast<int>(i) == c.b)
            continue;
        const double s = weightedScore(c.features[i], w);
        if (s > sb + 1e-12)
            ++better;
        else if (std::abs(s - sb) <= 1e-12)
            ++tied;
    }
    return 1 + better + tied / 2; // ties: the middle of them
}

void EvalMetrics::add(int rank)
{
    ++n;
    if (rank <= 0)
    {
        ++misses;
        return;
    }
    hit1 += rank <= 1 ? 1 : 0;
    hit5 += rank <= 5 ? 1 : 0;
    hit10 += rank <= 10 ? 1 : 0;
    reciprocal += 1.0 / rank;
}

double EvalMetrics::hitRate(int k) const noexcept
{
    if (n == 0)
        return 0.0;
    return static_cast<double>(k <= 1 ? hit1 : k <= 5 ? hit5 : hit10) / n;
}

EvalMetrics evaluate(const std::vector<ReplayCase>& cases, const Weights& w)
{
    EvalMetrics m;
    for (const auto& c : cases)
        m.add(rankOfB(c, w));
    return m;
}

std::vector<WeightSet> weightSets(const std::vector<Preset>& presets, const std::optional<PersonalModel>& personal)
{
    std::vector<WeightSet> out;
    for (const auto& p : presets)
        out.push_back({p.name, p.weights});
    const auto balanced = presets.empty() ? defaultPresets().front().weights : presets.front().weights;
    if (personal)
        out.push_back({"Personal (Balanced blended, a = " + fixed(blendAlpha(personal->impressions), 2) + ")",
                       blendWeights(balanced, personal->weights, personal->impressions)});
    for (int i = 0; i < kFeatureCount; ++i)
    {
        auto w = balanced;
        w.w[static_cast<std::size_t>(i)] = 0.0;
        out.push_back({std::string("Balanced - ") + featureName(static_cast<FeatureId>(i)), w});
    }
    return out;
}

Weights tuneWeights(const std::vector<ReplayCase>& cases, const Weights& start, int passes)
{
    auto best = start;
    double bestMrr = evaluate(cases, best).mrr();
    static constexpr double kSteps[] = {0.0, 0.5, 1.0, 1.5, 2.0, 3.0};
    for (int pass = 0; pass < passes; ++pass)
        for (int i = 0; i < kFeatureCount; ++i)
        {
            const double base = best.w[static_cast<std::size_t>(i)] > 0.0 ? best.w[static_cast<std::size_t>(i)]
                                                                            : start.w[static_cast<std::size_t>(i)];
            for (const double k : kSteps)
            {
                auto w = best;
                w.w[static_cast<std::size_t>(i)] = base * k;
                const double mrr = evaluate(cases, w).mrr();
                if (mrr > bestMrr + 1e-9)
                {
                    bestMrr = mrr;
                    best = w;
                }
            }
        }
    return best;
}

ChoiceStats choiceStats(library::Connection& db)
{
    ChoiceStats s;
    double rankSum = 0.0;
    for (const auto& m : ChoiceLogRepository(db).all())
    {
        ++s.impressions;
        if (m.chosenRank)
        {
            ++s.fromList;
            rankSum += *m.chosenRank;
        }
        s.thumbsUp += m.outcome == Outcome::ThumbsUp ? 1 : 0;
        s.thumbsDown += m.outcome == Outcome::ThumbsDown ? 1 : 0;
        s.mixed += m.outcome == Outcome::Mixed ? 1 : 0;
        s.ejected += m.outcome == Outcome::Ejected ? 1 : 0;
    }
    s.meanChosenRank = s.fromList > 0 ? rankSum / s.fromList : 0.0;
    return s;
}

std::string reportMarkdown(const EvalReport& r)
{
    std::ostringstream out;
    out << "# Track Matcher evaluation - " << r.date << "\n\n";
    out << "Generated by `tools/matcher_eval` (P6-13; MATCHER_SPEC §10) from the owner's library: " << r.tracks
        << " analyzed tracks, " << r.transitions << " transitions played (A then B) in " << r.sessions << " sessions.\n\n";
    if (r.transitions == 0)
        out << "**No transitions in the history yet** - play sets (a track counts as played after 60 s) and run it "
               "again.\n\n";
    out << "## Offline replay\n\n"
           "Each transition's B ranked among the library as it was then (tracks added later left out; history and play "
           "counts only from before it), default filters (tempo within 8 %, keys relaxed). A miss = B was filtered "
           "out.\n\n";
    out << "| Weights | HitRate@1 | @5 | @10 | MRR | misses |\n| --- | --- | --- | --- | --- | --- |\n";
    for (const auto& [name, m] : r.results)
        out << "| " << name << " | " << fixed(m.hitRate(1)) << " | " << fixed(m.hitRate(5)) << " | "
            << fixed(m.hitRate(10)) << " | " << fixed(m.mrr()) << " | " << m.misses << " |\n";
    if (r.tuned)
    {
        out << "\n## Tuned weights (a suggestion)\n\nCoordinate ascent on Balanced's weights for MRR, on the same "
               "transitions (in-sample: check it on the next sessions before trusting it). MRR "
            << fixed(r.tunedMetrics.mrr()) << ", HitRate@10 " << fixed(r.tunedMetrics.hitRate(10)) << ".\n\n";
        out << "```json\n{\"name\": \"Tuned\", \"weights\": {";
        for (int i = 0; i < kFeatureCount; ++i)
            out << (i > 0 ? ", " : "") << "\"" << featureName(static_cast<FeatureId>(i)) << "\": "
                << fixed(r.tuned->w[static_cast<std::size_t>(i)], 2);
        out << "}}\n```\n\nTo use it: run `matcher_eval --write-presets` (it adds \"Tuned\" to "
               "`Documents\\Devil-DJ\\Matcher\\presets.json`), restart Devil-DJ and pick Tuned in the Next Track panel.\n";
    }
    out << "\n## The choice log\n\n";
    const auto& c = r.choices;
    if (c.impressions == 0)
        out << "Nothing logged yet (Options > Track Matcher log).\n";
    else
    {
        out << "- Loads logged: " << c.impressions << "; taken from the suggestions: " << c.fromList << " ("
            << fixed(100.0 * c.fromList / c.impressions, 0) << " %), mean rank of those " << fixed(c.meanChosenRank, 1)
            << "\n";
        out << "- Outcomes: mixed " << c.mixed << ", ejected " << c.ejected << ", thumbs up " << c.thumbsUp
            << ", thumbs down " << c.thumbsDown;
        if (c.thumbsUp + c.thumbsDown > 0)
            out << " (" << fixed(100.0 * c.thumbsUp / (c.thumbsUp + c.thumbsDown), 0) << " % up)";
        out << "\n";
    }
    out << "\n## The personal ranker\n\n";
    if (!r.personal)
        out << "Not trained yet (it trains when Devil-DJ closes, from the choice log).\n";
    else
    {
        out << "Version " << r.personal->version << ", from " << r.personal->impressions << " choices (a = "
            << fixed(blendAlpha(r.personal->impressions), 2) << "). Its row above is in-sample where the logged choices "
               "and the history overlap.\n\n| Feature | Learnt weight |\n| --- | --- |\n";
        for (int i = 0; i < kRankDims; ++i)
            out << "| " << choiceDimName(i) << " | " << fixed(r.personal->weights[static_cast<std::size_t>(i)], 2) << " |\n";
    }
    out << "\nAblation rows: Balanced with that feature's weight set to 0 - a big drop in MRR means it matters for "
           "this library.\n";
    return out.str();
}

} // namespace devildj::matcher
