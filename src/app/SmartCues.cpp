#include "app/SmartCues.h"

#include "library/Connection.h"
#include "library/CueFeedbackRepository.h"
#include "library/CueRepository.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace devildj::app
{

namespace
{
constexpr double kRiseStep = 0.15; // an energy change this big is a rise / fall
}

const char* cueReasonName(CueReason reason) noexcept
{
    switch (reason)
    {
        case CueReason::Start:
            return "START";
        case CueReason::Rise:
            return "RISE";
        case CueReason::Drop:
            return "DROP";
        case CueReason::Break:
            return "BREAK";
        case CueReason::Outro:
            return "OUTRO";
        case CueReason::Phrase:
            break;
    }
    return "PHRASE";
}

CueReason cueReasonFromName(const std::string& name) noexcept
{
    for (const auto r : {CueReason::Start, CueReason::Rise, CueReason::Drop, CueReason::Break, CueReason::Outro})
        if (name == cueReasonName(r))
            return r;
    return CueReason::Phrase;
}

std::vector<CueCandidate> cueCandidates(const std::vector<library::StoredPhrase>& phrases, double duration)
{
    std::vector<CueCandidate> out;
    const int n = static_cast<int>(phrases.size());
    if (n == 0 || duration <= 0.0)
        return out;
    double maxEnergy = 0.0, biggestRise = 0.0;
    int drop = -1, outro = -1;
    for (int i = 0; i < n; ++i)
    {
        maxEnergy = std::max(maxEnergy, phrases[static_cast<std::size_t>(i)].energy);
        if (i > 0)
        {
            const double d = phrases[static_cast<std::size_t>(i)].energy - phrases[static_cast<std::size_t>(i - 1)].energy;
            if (d > biggestRise + 1e-12)
            {
                biggestRise = d;
                drop = i;
            }
            if (d <= -kRiseStep && phrases[static_cast<std::size_t>(i)].startSeconds >= duration * 2.0 / 3.0)
                outro = i; // the last such fall
        }
    }
    if (biggestRise < kRiseStep)
        drop = -1;
    for (int i = 0; i < n; ++i)
    {
        const auto& p = phrases[static_cast<std::size_t>(i)];
        const double before = i > 0 ? phrases[static_cast<std::size_t>(i - 1)].energy : 0.0;
        const double delta = i > 0 ? p.energy - before : p.energy;
        CueCandidate c;
        c.seconds = p.startSeconds;
        c.reason = i == 0          ? CueReason::Start
                   : i == drop     ? CueReason::Drop
                   : i == outro    ? CueReason::Outro
                   : delta >= kRiseStep  ? CueReason::Rise
                   : delta <= -kRiseStep ? CueReason::Break
                                         : CueReason::Phrase;
        c.features = {p.startSeconds / duration,
                      p.energy,
                      before,
                      delta,
                      i == 0 ? 1.0 : 0.0,
                      1.0 / (1.0 + (n - 1 - i)), // 1 on the last phrase, smaller before it
                      i == drop ? 1.0 : 0.0,
                      std::min(1.0, p.bars / 32.0),
                      maxEnergy > 0.0 ? p.energy / maxEnergy : 0.0};
        switch (c.reason)
        {
            case CueReason::Start:
                c.rule = 0.9;
                break;
            case CueReason::Drop:
                c.rule = 0.85;
                break;
            case CueReason::Outro:
                c.rule = 0.6;
                break;
            case CueReason::Rise:
                c.rule = std::min(0.8, 0.45 + delta);
                break;
            case CueReason::Break:
                c.rule = std::min(0.7, 0.4 - delta * 0.5);
                break;
            case CueReason::Phrase:
                c.rule = 0.15;
                break;
        }
        c.score = c.rule;
        out.push_back(c);
    }
    return out;
}

void CueHabits::train(const std::vector<Example>& examples)
{
    pos = neg = 0;
    for (const auto& e : examples)
        (e.cued ? pos : neg)++;
    ready = pos >= kMinEach && neg >= kMinEach;
    weights.fill(0.0);
    bias = 0.0;
    if (!ready)
        return;
    // Gradient descent on the log loss with L2, classes weighted to balance (few cues among many phrases).
    const double wPos = 0.5 * static_cast<double>(pos + neg) / pos, wNeg = 0.5 * static_cast<double>(pos + neg) / neg;
    constexpr double kRate = 0.5, kL2 = 0.01;
    for (int epoch = 0; epoch < 400; ++epoch)
    {
        CueFeatureVector grad{};
        double gradBias = 0.0, total = 0.0;
        for (const auto& e : examples)
        {
            const double p = probability(e.features);
            const double w = e.cued ? wPos : wNeg;
            const double err = (p - (e.cued ? 1.0 : 0.0)) * w;
            for (int k = 0; k < kCueFeatures; ++k)
                grad[static_cast<std::size_t>(k)] += err * e.features[static_cast<std::size_t>(k)];
            gradBias += err;
            total += w;
        }
        for (int k = 0; k < kCueFeatures; ++k)
            weights[static_cast<std::size_t>(k)] -= kRate * (grad[static_cast<std::size_t>(k)] / total + kL2 * weights[static_cast<std::size_t>(k)]);
        bias -= kRate * gradBias / total;
    }
}

double CueHabits::probability(const CueFeatureVector& f) const noexcept
{
    double z = bias;
    for (int k = 0; k < kCueFeatures; ++k)
        z += weights[static_cast<std::size_t>(k)] * f[static_cast<std::size_t>(k)];
    return 1.0 / (1.0 + std::exp(-z));
}

std::vector<CueHabits::Example> cueExamples(const std::vector<CueCandidate>& candidates,
                                            const std::vector<double>& cuedSeconds, double tolerance)
{
    std::vector<CueHabits::Example> out;
    for (const auto& c : candidates)
    {
        const bool cued = std::any_of(cuedSeconds.begin(), cuedSeconds.end(),
                                      [&](double s) { return std::abs(s - c.seconds) <= tolerance; });
        out.push_back({c.features, cued});
    }
    return out;
}

std::vector<CueCandidate> suggestCues(const std::vector<library::StoredPhrase>& phrases, double duration,
                                      const CueHabits& habits, const std::vector<double>& existing,
                                      const std::vector<double>& rejected, double tolerance, int maxCount)
{
    auto all = cueCandidates(phrases, duration);
    const auto near = [tolerance](const std::vector<double>& list, double s)
    { return std::any_of(list.begin(), list.end(), [&](double x) { return std::abs(x - s) <= tolerance; }); };
    std::vector<CueCandidate> kept;
    for (auto& c : all)
    {
        if (habits.trained())
            c.score = 0.5 * c.rule + 0.5 * habits.probability(c.features);
        if (c.score >= kMinScore && !near(existing, c.seconds) && !near(rejected, c.seconds))
            kept.push_back(c);
    }
    std::stable_sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    if (static_cast<int>(kept.size()) > maxCount)
        kept.resize(static_cast<std::size_t>(std::max(0, maxCount)));
    std::sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return a.seconds < b.seconds; });
    return kept;
}

std::vector<CueHabits::Example> gatherCueExamples(library::Connection& db, double tolerance)
{
    std::vector<CueHabits::Example> out;
    std::map<std::int64_t, std::vector<library::CueAnswer>> answers;
    for (auto& a : library::CueFeedbackRepository(db).all())
        answers[a.trackId].push_back(std::move(a));
    std::vector<std::int64_t> tracks;
    if (auto* s = db.prepare("SELECT DISTINCT track_id FROM phrases"))
    {
        while (s->step())
            tracks.push_back(s->getInt(0));
        s->reset();
    }
    library::PhraseRepository phrases(db);
    library::CueRepository cues(db);
    for (const auto id : tracks)
    {
        double rate = 0.0, duration = 0.0;
        if (auto* s = db.prepare("SELECT sample_rate, duration_ms FROM tracks WHERE id = ?"))
        {
            if (s->bind(1, id).step())
            {
                rate = static_cast<double>(s->getOptionalInt(0).value_or(0));
                duration = static_cast<double>(s->getOptionalInt(1).value_or(0)) / 1000.0;
            }
            s->reset();
        }
        std::vector<double> cued, rejected;
        if (rate > 0.0)
            for (const auto& c : cues.forTrack(id))
                if (c.isHot())
                    cued.push_back(static_cast<double>(c.startSample) / rate);
        if (const auto it = answers.find(id); it != answers.end())
            for (const auto& a : it->second)
                (a.accepted ? cued : rejected).push_back(a.startSeconds);
        if (cued.empty() && rejected.empty())
            continue; // not looked at: no labels
        const auto stored = phrases.get(id);
        if (duration <= 0.0 && !stored.phrases.empty())
            duration = stored.phrases.back().startSeconds + 1.0;
        for (auto& e : cueExamples(cueCandidates(stored.phrases, duration), cued, tolerance))
            out.push_back(e);
        // A rejected candidate is not cued even if a cue happens to be near it.
        const auto candidates = cueCandidates(stored.phrases, duration);
        for (std::size_t i = 0; i < candidates.size(); ++i)
            for (const double r : rejected)
                if (std::abs(candidates[i].seconds - r) <= tolerance)
                    out[out.size() - candidates.size() + i].cued = false;
    }
    return out;
}

std::array<int, 8> padsForSuggestions(const std::array<bool, 8>& slotSet, int count)
{
    std::array<int, 8> out;
    out.fill(-1);
    int next = 0;
    for (int slot = 0; slot < 8 && next < count; ++slot)
        if (!slotSet[static_cast<std::size_t>(slot)])
            out[static_cast<std::size_t>(slot)] = next++;
    return out;
}

} // namespace devildj::app
