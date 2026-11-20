#include "FeatureNormalizer.h"

#include "analysis/TrackFeatures.h"
#include "library/Connection.h"

#include <algorithm>
#include <cmath>

namespace devildj::analysis
{

int normalizeLibraryFeatures(library::Connection& db, std::string* error)
{
    const auto fail = [&](const std::string& why)
    {
        if (error != nullptr)
            *error = why;
        return -1;
    };
    struct Row
    {
        std::int64_t id = 0;
        std::vector<EnergyWindow> windows;
        std::vector<float> sound;
    };
    std::vector<Row> rows;
    {
        auto* s = db.prepare("SELECT track_id, energy_features, sound_v1 FROM track_features");
        if (s == nullptr)
            return fail(db.lastError());
        while (s->step())
            rows.push_back({s->getInt(0), unpackWindows(s->getBlob(1)), unpackFloats(s->getBlob(2))});
    }

    // Every window of the library: z statistics, then each window's percentile.
    std::vector<EnergyWindow> all;
    for (const auto& r : rows)
        all.insert(all.end(), r.windows.begin(), r.windows.end());
    const auto stats = energyStats(all);
    std::vector<double> scores;
    scores.reserve(all.size());
    for (const auto& w : all)
        scores.push_back(rawEnergy(w, stats));
    std::sort(scores.begin(), scores.end());
    const auto percentile = [&](double raw)
    {
        if (scores.size() < 2)
            return 0.5;
        // Mid-rank: ties share the middle of their ranks.
        const auto lo = std::lower_bound(scores.begin(), scores.end(), raw) - scores.begin();
        const auto hi = std::upper_bound(scores.begin(), scores.end(), raw) - scores.begin();
        return std::clamp((0.5 * static_cast<double>(lo + hi) - 0.5) / static_cast<double>(scores.size() - 1), 0.0, 1.0);
    };

    // Sound vectors: per dimension mean and standard deviation.
    std::vector<double> soundSum(kSoundDims, 0.0), soundSq(kSoundDims, 0.0);
    int soundCount = 0;
    for (const auto& r : rows)
    {
        if (r.sound.size() != static_cast<std::size_t>(kSoundDims))
            continue;
        for (int i = 0; i < kSoundDims; ++i)
        {
            soundSum[static_cast<std::size_t>(i)] += r.sound[static_cast<std::size_t>(i)];
            soundSq[static_cast<std::size_t>(i)] += r.sound[static_cast<std::size_t>(i)] * r.sound[static_cast<std::size_t>(i)];
        }
        ++soundCount;
    }

    int updated = 0;
    const bool ok = db.savepoint(
        [&]
        {
            auto* u = db.prepare("UPDATE track_features SET energy = ?, energy_intro = ?, energy_outro = ?, "
                                 "energy_curve = ? WHERE track_id = ?");
            if (u == nullptr)
                return false;
            for (const auto& r : rows)
            {
                if (r.windows.empty())
                    continue;
                std::vector<float> curve;
                double sum = 0.0;
                for (const auto& w : r.windows)
                {
                    curve.push_back(static_cast<float>(percentile(rawEnergy(w, stats))));
                    sum += curve.back();
                }
                const auto blob = packHalves(curve);
                u->bind(1, sum / static_cast<double>(curve.size())).bind(2, static_cast<double>(curve.front()));
                u->bind(3, static_cast<double>(curve.back())).bind(4, std::span<const std::uint8_t>(blob)).bind(5, r.id);
                if (!u->run())
                    return false;
                ++updated;
            }
            auto* st = db.prepare("INSERT INTO feature_stats (name, data) VALUES (?, ?) "
                                  "ON CONFLICT(name) DO UPDATE SET data = excluded.data");
            if (st == nullptr)
                return false;
            std::vector<float> energy;
            for (const double v : stats.mean)
                energy.push_back(static_cast<float>(v));
            for (const double v : stats.sd)
                energy.push_back(static_cast<float>(v));
            const auto energyBlob = packFloats(energy);
            if (!st->bind(1, "energy").bind(2, std::span<const std::uint8_t>(energyBlob)).run())
                return false;
            if (soundCount > 0)
            {
                std::vector<float> mean(kSoundDims), sd(kSoundDims);
                for (int i = 0; i < kSoundDims; ++i)
                {
                    const double m = soundSum[static_cast<std::size_t>(i)] / soundCount;
                    mean[static_cast<std::size_t>(i)] = static_cast<float>(m);
                    sd[static_cast<std::size_t>(i)] = static_cast<float>(
                        std::max(1e-6, std::sqrt(std::max(0.0, soundSq[static_cast<std::size_t>(i)] / soundCount - m * m))));
                }
                const auto meanBlob = packFloats(mean), sdBlob = packFloats(sd);
                if (!st->bind(1, "sound_mean").bind(2, std::span<const std::uint8_t>(meanBlob)).run())
                    return false;
                if (!st->bind(1, "sound_sd").bind(2, std::span<const std::uint8_t>(sdBlob)).run())
                    return false;
            }
            return true;
        });
    if (!ok)
        return fail(db.lastError());
    return updated;
}

} // namespace devildj::analysis
