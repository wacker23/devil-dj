#include "matcher/CandidateQuery.h"

#include "analysis/TrackFeatures.h"
#include "library/Connection.h"

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
