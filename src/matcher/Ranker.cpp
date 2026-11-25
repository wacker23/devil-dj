#include "matcher/Ranker.h"

#include "library/Connection.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace devildj::matcher
{

RankWeights rankWeightsOf(const Weights& preset)
{
    RankWeights w{};
    for (int i = 0; i < kFeatureCount; ++i)
        w[static_cast<std::size_t>(i)] = preset.w[static_cast<std::size_t>(i)];
    return w;
}

Weights weightsOf(const RankWeights& r)
{
    Weights w;
    for (int i = 0; i < kFeatureCount; ++i)
        w.w[static_cast<std::size_t>(i)] = r[static_cast<std::size_t>(i)];
    for (int j = 0; j < 3; ++j)
        w.interactions[static_cast<std::size_t>(j)] = r[static_cast<std::size_t>(kFeatureCount + j)];
    return w;
}

std::vector<RankPair> rankPairs(const std::vector<Impression>& impressions)
{
    std::vector<RankPair> out;
    for (const auto& m : impressions)
    {
        const ShownItem* chosen = nullptr;
        for (const auto& s : m.shown)
            if (s.chosen)
                chosen = &s;
        if (chosen == nullptr)
            continue;
        double weight = 1.0;
        bool reversed = false;
        switch (m.outcome)
        {
            case Outcome::Mixed:
            case Outcome::ThumbsUp:
                weight = 2.0;
                break;
            case Outcome::Ejected:
                weight = 0.5;
                reversed = true;
                break;
            case Outcome::ThumbsDown:
                weight = 2.0;
                reversed = true;
                break;
            case Outcome::Unknown:
                break;
        }
        for (const auto& c : m.shown)
        {
            if (&c == chosen)
                continue;
            RankPair p;
            p.weight = weight;
            for (int k = 0; k < kRankDims; ++k)
            {
                const auto& a = chosen->x[static_cast<std::size_t>(k)];
                const auto& b = c.x[static_cast<std::size_t>(k)];
                const double diff = a && b ? *a - *b : 0.0; // a missing value says nothing
                p.d[static_cast<std::size_t>(k)] = reversed ? -diff : diff;
            }
            out.push_back(p);
        }
    }
    return out;
}

TrainResult trainRanker(const std::vector<RankPair>& pairs, const RankWeights& prior, const TrainOptions& o)
{
    const auto t0 = std::chrono::steady_clock::now();
    TrainResult result;
    result.pairs = static_cast<int>(pairs.size());
    RankWeights w = prior;
    double totalWeight = 0.0;
    for (const auto& p : pairs)
        totalWeight += p.weight;
    const double scale = 1.0 / (totalWeight + o.lambda); // keeps the steps the same size whatever the data
    // Adam, full batch, projected onto w >= 0.
    std::array<double, kRankDims> m{}, v{};
    constexpr double kBeta1 = 0.9, kBeta2 = 0.999, kEps = 1e-8;
    for (int it = 1; it <= o.iterations; ++it)
    {
        std::array<double, kRankDims> grad{};
        for (const auto& p : pairs)
        {
            double z = 0.0;
            for (int k = 0; k < kRankDims; ++k)
                z += w[static_cast<std::size_t>(k)] * p.d[static_cast<std::size_t>(k)];
            const double g = -p.weight / (1.0 + std::exp(z)); // d/dz of -weight log sigmoid(z)
            for (int k = 0; k < kRankDims; ++k)
                grad[static_cast<std::size_t>(k)] += g * p.d[static_cast<std::size_t>(k)];
        }
        for (int k = 0; k < kRankDims; ++k)
        {
            const auto i = static_cast<std::size_t>(k);
            const double gk = scale * (grad[i] + 2.0 * o.lambda * (w[i] - prior[i]));
            m[i] = kBeta1 * m[i] + (1.0 - kBeta1) * gk;
            v[i] = kBeta2 * v[i] + (1.0 - kBeta2) * gk * gk;
            const double mh = m[i] / (1.0 - std::pow(kBeta1, it)), vh = v[i] / (1.0 - std::pow(kBeta2, it));
            w[i] = std::max(0.0, w[i] - o.learningRate * mh / (std::sqrt(vh) + kEps));
        }
    }
    double loss = 0.0;
    for (const auto& p : pairs)
    {
        double z = 0.0;
        for (int k = 0; k < kRankDims; ++k)
            z += w[static_cast<std::size_t>(k)] * p.d[static_cast<std::size_t>(k)];
        loss += p.weight * std::log1p(std::exp(-z));
    }
    result.loss = totalWeight > 0.0 ? loss / totalWeight : 0.0;
    // To the prior's scale (the 11 features' sum), so blending and the view compare like with like.
    double sumW = 0.0, sumPrior = 0.0;
    for (int k = 0; k < kFeatureCount; ++k)
    {
        sumW += w[static_cast<std::size_t>(k)];
        sumPrior += prior[static_cast<std::size_t>(k)];
    }
    if (sumW > 1e-12 && sumPrior > 0.0)
        for (auto& x : w)
            x *= sumPrior / sumW;
    result.weights = w;
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return result;
}

double blendAlpha(int n) noexcept
{
    return n > 0 ? static_cast<double>(n) / (n + 200.0) : 0.0;
}

Weights blendWeights(const Weights& preset, const RankWeights& learnt, int impressions)
{
    const double a = blendAlpha(impressions);
    const auto p = rankWeightsOf(preset);
    RankWeights w{};
    for (int k = 0; k < kRankDims; ++k)
        w[static_cast<std::size_t>(k)] = (1.0 - a) * p[static_cast<std::size_t>(k)] + a * learnt[static_cast<std::size_t>(k)];
    return weightsOf(w);
}

std::optional<PersonalModel> loadPersonalModel(library::Connection& db)
{
    auto* s = db.prepare("SELECT version, weights_json, trained_at, n_impressions FROM matcher_models WHERE name = ?");
    if (s == nullptr || !s->bind(1, kPersonalModelName).step())
        return std::nullopt;
    PersonalModel m;
    m.version = static_cast<int>(s->getInt(0));
    const auto json = s->getText(1);
    m.trainedAt = s->getInt(2);
    m.impressions = static_cast<int>(s->getInt(3));
    s->reset();
    // {"based_on": "...", "w": [14 numbers]}
    if (const auto b = json.find("\"based_on\""); b != std::string::npos)
    {
        const auto q1 = json.find('"', json.find(':', b) + 1), q2 = json.find('"', q1 + 1);
        if (q1 != std::string::npos && q2 != std::string::npos)
            m.basedOn = json.substr(q1 + 1, q2 - q1 - 1);
    }
    auto at = json.find('[', json.find("\"w\""));
    for (int k = 0; k < kRankDims && at != std::string::npos; ++k)
    {
        char* end = nullptr;
        m.weights[static_cast<std::size_t>(k)] = std::max(0.0, std::strtod(json.c_str() + at + 1, &end));
        at = json.find_first_of(",]", static_cast<std::size_t>(end - json.c_str()));
        if (at == std::string::npos || json[at] == ']')
            break;
    }
    return m;
}

bool savePersonalModel(library::Connection& db, const PersonalModel& m)
{
    std::string json = "{\"based_on\": \"" + m.basedOn + "\", \"dims\": [";
    for (int k = 0; k < kRankDims; ++k)
        json += std::string(k > 0 ? ", " : "") + "\"" + choiceDimName(k) + "\"";
    json += "], \"w\": [";
    for (int k = 0; k < kRankDims; ++k)
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%s%.6g", k > 0 ? ", " : "", m.weights[static_cast<std::size_t>(k)]);
        json += buffer;
    }
    json += "]}";
    auto* s = db.prepare("INSERT INTO matcher_models (name, version, weights_json, trained_at, n_impressions) VALUES "
                         "(?, ?, ?, ?, ?) ON CONFLICT(name) DO UPDATE SET version = excluded.version, weights_json = "
                         "excluded.weights_json, trained_at = excluded.trained_at, n_impressions = excluded.n_impressions");
    return s != nullptr && s->bind(1, kPersonalModelName).bind(2, m.version).bind(3, json).bind(4, m.trainedAt)
                               .bind(5, m.impressions).run();
}

bool resetPersonalModel(library::Connection& db)
{
    auto* s = db.prepare("DELETE FROM matcher_models WHERE name = ?");
    return s != nullptr && s->bind(1, kPersonalModelName).run();
}

std::optional<PersonalModel> retrainPersonalModel(library::Connection& db, const Weights& preset,
                                                  const std::string& presetName, std::int64_t now)
{
    const auto impressions = ChoiceLogRepository(db).all();
    const auto pairs = rankPairs(impressions);
    if (pairs.empty())
        return std::nullopt;
    const auto trained = trainRanker(pairs, rankWeightsOf(preset));
    PersonalModel m;
    m.weights = trained.weights;
    m.version = loadPersonalModel(db).value_or(PersonalModel{}).version + 1;
    m.trainedAt = now;
    m.impressions = static_cast<int>(impressions.size());
    m.basedOn = presetName;
    if (!savePersonalModel(db, m))
        return std::nullopt;
    return m;
}

} // namespace devildj::matcher
