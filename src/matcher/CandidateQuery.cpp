#include "matcher/CandidateQuery.h"

#include "analysis/TrackFeatures.h"
#include "library/Connection.h"
#include "util/MixRegions.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>

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

// The library's sound statistics (feature_stats 'sound_mean' / 'sound_sd', P2-24) to z-normalize sound vectors.
struct SoundStats
{
    std::vector<float> mean, sd;
};

SoundStats soundStats(library::Connection& db)
{
    SoundStats s;
    if (auto* st = db.prepare("SELECT name, data FROM feature_stats WHERE name IN ('sound_mean', 'sound_sd')"))
        while (st->step())
            (st->getText(0) == "sound_mean" ? s.mean : s.sd) = analysis::unpackFloats(st->getBlob(1));
    return s;
}

std::vector<float> normalized(std::vector<float> v, const SoundStats& stats)
{
    if (v.size() != stats.mean.size() || v.size() != stats.sd.size())
        return v;
    for (std::size_t i = 0; i < v.size(); ++i)
        v[i] = stats.sd[i] > 1e-9f ? (v[i] - stats.mean[i]) / stats.sd[i] : 0.0f;
    return v;
}

const char* kColumns =
    "SELECT t.id, t.content_hash, t.artist, t.genre, t.bpm, t.musical_key, t.rating, "
    "f.energy, f.energy_intro, f.energy_outro, f.sound_v1, f.groove FROM tracks t "
    "LEFT JOIN track_features f ON f.track_id = t.id ";

TrackInfo rowToTrack(library::Statement& st, const SoundStats& stats)
{
    TrackInfo t;
    t.id = st.getInt(0);
    t.contentHash = st.getText(1);
    t.artist = st.getText(2);
    if (const auto genre = st.getText(3); !genre.empty())
        t.tags.push_back(genre);
    t.bpm = st.getDouble(4);
    t.key = static_cast<int>(st.getInt(5));
    t.rating = static_cast<int>(st.getInt(6));
    t.energy = st.getOptionalDouble(7);
    t.energyIntro = st.getOptionalDouble(8);
    t.energyOutro = st.getOptionalDouble(9);
    t.sound = normalized(analysis::unpackFloats(st.getBlob(10)), stats);
    t.groove = analysis::unpackFloats(st.getBlob(11));
    return t;
}

// A region's energy: the 0..1 energy curve (per energy window) averaged over [t0, t1], by overlap.
std::optional<double> regionEnergy(const std::vector<analysis::EnergyWindow>& windows, const std::vector<float>& curve,
                                   double t0, double t1)
{
    double sum = 0.0, weight = 0.0;
    for (std::size_t i = 0; i < std::min(windows.size(), curve.size()); ++i)
    {
        const double a = std::max(t0, static_cast<double>(windows[i].startSeconds));
        const double b = std::min(t1, static_cast<double>(windows[i].startSeconds + windows[i].seconds));
        if (b > a)
        {
            sum += curve[i] * (b - a);
            weight += b - a;
        }
    }
    if (weight <= 0.0)
        return std::nullopt;
    return sum / weight;
}

// P6-10: phrases, mix points, the energy curve, the region sound vectors and vocal presence -> the regions' values.
void addStructure(library::Connection& db, std::vector<TrackInfo>& tracks, const SoundStats& stats)
{
    std::unordered_map<std::int64_t, std::size_t> index;
    for (std::size_t i = 0; i < tracks.size(); ++i)
        index[tracks[i].id] = i;
    std::unordered_map<std::int64_t, std::vector<util::PhraseSpan>> phrases;
    if (auto* st = db.prepare("SELECT track_id, start_seconds, bars FROM phrases ORDER BY track_id, idx"))
        while (st->step())
            if (index.count(st->getInt(0)) > 0)
                phrases[st->getInt(0)].push_back({st->getDouble(1), static_cast<int>(st->getInt(2))});
    std::unordered_map<std::int64_t, std::pair<std::optional<double>, std::optional<double>>> points; // in, out
    if (auto* st = db.prepare("SELECT c.track_id, c.kind, c.start_sample, t.sample_rate FROM cues c JOIN tracks t ON "
                              "t.id = c.track_id WHERE c.kind IN (4, 5) AND t.sample_rate > 0"))
        while (st->step())
            if (index.count(st->getInt(0)) > 0)
            {
                const double seconds = static_cast<double>(st->getInt(2)) / static_cast<double>(st->getInt(3));
                auto& p = points[st->getInt(0)];
                (st->getInt(1) == 4 ? p.first : p.second) = seconds;
            }
    if (auto* st = db.prepare("SELECT track_id, segment, vec FROM track_embeddings WHERE model = 'sound_v1' AND "
                              "segment IN ('intro', 'outro')"))
        while (st->step())
            if (const auto it = index.find(st->getInt(0)); it != index.end())
                (st->getText(1) == "intro" ? tracks[it->second].soundIntro : tracks[it->second].soundOutro) =
                    normalized(analysis::unpackFloats(st->getBlob(2)), stats);
    if (auto* st = db.prepare("SELECT t.id, t.duration_ms, f.energy_features, f.energy_curve, f.vocal_intro, "
                              "f.vocal_outro FROM tracks t LEFT JOIN track_features f ON f.track_id = t.id"))
        while (st->step())
        {
            const auto it = index.find(st->getInt(0));
            if (it == index.end())
                continue;
            auto& t = tracks[it->second];
            t.vocalIntro = st->getOptionalDouble(4);
            t.vocalOutro = st->getOptionalDouble(5);
            const double duration = static_cast<double>(st->getOptionalInt(1).value_or(0)) / 1000.0;
            const auto& ph = phrases[t.id];
            const auto& pt = points[t.id];
            const auto r = util::mixRegions(ph, pt.first, pt.second, t.bpm, duration);
            t.introBars = r.introBars;
            t.outroBars = r.outroBars;
            if (!ph.empty() || pt.second || duration > 0.0)
                t.outStartSeconds = r.outStart;
            const auto windows = analysis::unpackWindows(st->getBlob(2));
            const auto curve = analysis::unpackHalves(st->getBlob(3));
            t.energyIn = regionEnergy(windows, curve, r.inStart, r.inEnd);
            if (duration > 0.0 || !ph.empty() || pt.second)
                t.energyOut = regionEnergy(windows, curve, r.outStart, r.outEnd);
        }
}

void addTagsAndPlays(library::Connection& db, std::vector<TrackInfo>& tracks, std::int64_t now)
{
    std::unordered_map<std::int64_t, std::size_t> index;
    for (std::size_t i = 0; i < tracks.size(); ++i)
        index[tracks[i].id] = i;
    if (auto* st = db.prepare("SELECT tt.track_id, g.name FROM track_tags tt JOIN tags g ON g.id = tt.tag_id"))
        while (st->step())
            if (const auto it = index.find(st->getInt(0)); it != index.end())
                tracks[it->second].tags.push_back(st->getText(1));
    if (auto* st = db.prepare("SELECT track_id, COUNT(*) FROM history WHERE played_at >= ? GROUP BY track_id"))
    {
        st->bind(1, now - kMonth);
        while (st->step())
            if (const auto it = index.find(st->getInt(0)); it != index.end())
                tracks[it->second].playsLast30Days = static_cast<int>(st->getInt(1));
    }
}
} // namespace

std::vector<TrackInfo> loadCandidates(library::Connection& db, const Scope& scope, std::int64_t exclude, int minRating,
                                      std::int64_t now)
{
    // The hard filters SQL can do (§3.1, 3.2 same track, 3.5): present, analyzed, not A, rated enough, in scope.
    std::string sql = std::string(kColumns) +
                      "WHERE t.missing = 0 AND t.bpm IS NOT NULL AND t.bpm > 0 AND t.musical_key IS NOT NULL "
                      "AND t.id != ? AND t.rating >= ?";
    if (scope.playlistId != 0)
        sql += " AND t.id IN (SELECT track_id FROM playlist_tracks WHERE playlist_id = ?)";
    std::vector<TrackInfo> out;
    auto* st = db.prepare(sql);
    if (st == nullptr)
        return out;
    st->bind(1, exclude).bind(2, minRating);
    if (scope.playlistId != 0)
        st->bind(3, scope.playlistId);
    const auto stats = soundStats(db);
    while (st->step())
        out.push_back(rowToTrack(*st, stats));
    addTagsAndPlays(db, out, now);
    addStructure(db, out, stats); // P6-10
    return out;
}

std::optional<TrackInfo> loadTrack(library::Connection& db, std::int64_t trackId, std::int64_t now)
{
    auto* st = db.prepare(std::string(kColumns) + "WHERE t.id = ? AND t.bpm IS NOT NULL AND t.musical_key IS NOT NULL");
    if (st == nullptr)
        return std::nullopt;
    st->bind(1, trackId);
    if (!st->step())
        return std::nullopt;
    const auto stats = soundStats(db);
    std::vector<TrackInfo> one{rowToTrack(*st, stats)};
    st->reset();
    addTagsAndPlays(db, one, now);
    addStructure(db, one, stats); // P6-10
    return one.front();
}

void loadHistory(library::Connection& db, Context& c, std::int64_t now)
{
    c.pairCounts.clear();
    c.artistPairCounts.clear();
    if (auto* st = db.prepare("SELECT track_id, COUNT(*) FROM history WHERE previous_track_id = ? GROUP BY track_id"))
    {
        st->bind(1, c.a.id);
        while (st->step())
            c.pairCounts[st->getInt(0)] = static_cast<int>(st->getInt(1));
    }
    if (!c.a.artist.empty())
        if (auto* st = db.prepare("SELECT t.artist, COUNT(*) FROM history h JOIN tracks p ON p.id = h.previous_track_id "
                                  "JOIN tracks t ON t.id = h.track_id WHERE p.artist = ? GROUP BY t.artist"))
        {
            st->bind(1, c.a.artist);
            while (st->step())
                c.artistPairCounts[lower(st->getText(0))] = static_cast<int>(st->getInt(1));
        }
    if (auto* st = db.prepare("SELECT MAX(n) FROM (SELECT COUNT(*) AS n FROM history WHERE previous_track_id IS NOT NULL "
                              "GROUP BY previous_track_id, track_id)"))
        if (st->step())
            c.maxPairCount = static_cast<int>(st->getInt(0));
    if (auto* st = db.prepare("SELECT MAX(n) FROM (SELECT COUNT(*) AS n FROM history WHERE played_at >= ? GROUP BY track_id)"))
    {
        st->bind(1, now - kMonth);
        if (st->step())
            c.maxPlaysLast30Days = static_cast<int>(st->getInt(0));
    }
}

} // namespace devildj::matcher
