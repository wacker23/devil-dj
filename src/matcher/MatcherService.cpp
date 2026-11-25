#include "matcher/MatcherService.h"

#include "library/Database.h"

#include <chrono>
#include <cstdio>

namespace devildj::matcher
{

MatcherService::MatcherService(library::Database& db, std::vector<Preset> p)
    : database(db)
    , presets(std::move(p))
    , worker([this] { loop(); })
{
}

MatcherService::~MatcherService()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        quit = true;
    }
    wake.notify_all();
    worker.join();
}

void MatcherService::request(MatchRequest r)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending = std::move(r); // replaces one still waiting
    }
    wake.notify_all();
}

void MatcherService::loop()
{
    std::unique_ptr<library::Connection> reader;
    for (;;)
    {
        MatchRequest r;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [this] { return quit || pending.has_value(); });
            if (quit)
                return;
            r = std::move(*pending);
            pending.reset();
        }
        if (reader == nullptr)
            if (auto opened = database.openReader())
                reader = std::move(opened).value();
        if (reader == nullptr)
            continue;
        const auto result = run(*reader, r, presets);
        if (onResult)
            onResult(result);
    }
}

namespace
{
Context contextFor(library::Connection& db, const MatchRequest& r, const TrackInfo& a)
{
    Context c;
    c.a = a;
    c.aInOutro = r.aInOutro;
    c.aBpmNow = r.aBpmNow > 0.0 ? r.aBpmNow : a.bpm;
    c.keyLockOff = r.keyLockOff;
    c.allowKeyShift = r.allowKeyShift;
    c.intent = r.intent;
    c.maxStretch = r.maxStretch;
    c.keyPolicy = r.keyPolicy;
    c.playedPolicy = r.playedPolicy;
    c.playedThisSession = r.playedThisSession;
    c.lastArtists = r.lastArtists;
    c.minRating = r.minRating;
    loadHistory(db, c, r.nowUnixSeconds);
    return c;
}
} // namespace

std::optional<Suggestion> MatcherService::scoreOne(library::Connection& db, const MatchRequest& r, std::int64_t trackId,
                                                    const std::vector<Preset>& presets)
{
    const auto a = loadTrack(db, r.contextTrackId, r.nowUnixSeconds);
    const auto b = loadTrack(db, trackId, r.nowUnixSeconds);
    if (!a || !b)
        return std::nullopt;
    const auto c = contextFor(db, r, *a);
    const auto* preset = findPreset(presets, r.preset);
    const auto s = score(c, *b, r.weights ? *r.weights : preset != nullptr ? preset->weights : defaultPresets().front().weights);
    Suggestion out;
    out.trackId = trackId;
    out.score = s.score;
    for (std::size_t i = 0; i < out.features.size(); ++i)
        out.features[i] = s.features[i].value;
    out.energyDelta = energyDelta(c, *b);
    return out;
}

MatchResult MatcherService::run(library::Connection& db, const MatchRequest& r, const std::vector<Preset>& presets)
{
    const auto t0 = std::chrono::steady_clock::now();
    MatchResult result;
    result.contextTrackId = r.contextTrackId;
    result.request = r;
    const auto a = loadTrack(db, r.contextTrackId, r.nowUnixSeconds);
    if (!a)
        return result;
    Context c;
    c.a = *a;
    c.aInOutro = r.aInOutro;
    c.aBpmNow = r.aBpmNow > 0.0 ? r.aBpmNow : a->bpm;
    c.keyLockOff = r.keyLockOff;
    c.allowKeyShift = r.allowKeyShift;
    c.intent = r.intent;
    c.maxStretch = r.maxStretch;
    c.keyPolicy = r.keyPolicy;
    c.playedPolicy = r.playedPolicy;
    c.playedThisSession = r.playedThisSession;
    c.lastArtists = r.lastArtists;
    c.minRating = r.minRating;
    loadHistory(db, c, r.nowUnixSeconds);
    const auto candidates = loadCandidates(db, r.scope, r.contextTrackId, r.minRating, r.nowUnixSeconds);
    result.candidates = static_cast<int>(candidates.size());
    const auto [c10, c90] = soundPercentiles(candidates);
    c.c10 = c10;
    c.c90 = c90;
    const auto* preset = findPreset(presets, r.preset);
    const Weights weights = r.weights ? *r.weights : preset != nullptr ? preset->weights : defaultPresets().front().weights;
    for (const auto& s : rank(c, candidates, weights, 10))
    {
        Suggestion out;
        out.trackId = s.track->id;
        out.score = s.score;
        out.wildcard = s.wildcard;
        out.reasons = s.reasons;
        for (std::size_t i = 0; i < out.features.size(); ++i)
            out.features[i] = s.features[i].value;
        out.energyDelta = energyDelta(c, *s.track);
        for (const auto id : {FeatureId::Key, FeatureId::Tempo, FeatureId::Energy, FeatureId::Sound, FeatureId::Groove,
                              FeatureId::History})
        {
            const auto& f = s.features[static_cast<std::size_t>(id)];
            if (f.value && !f.reason.empty() && out.chips.size() < 4)
                out.chips.emplace_back(f.reason, *f.value);
        }
        for (int i = 0; i < kFeatureCount; ++i)
        {
            const auto& f = s.features[static_cast<std::size_t>(i)];
            char line[160];
            if (f.value)
                std::snprintf(line, sizeof(line), "%-8s %.2f x %.1f  %s\n", featureName(static_cast<FeatureId>(i)),
                              *f.value, weights.w[static_cast<std::size_t>(i)], f.reason.c_str());
            else
                std::snprintf(line, sizeof(line), "%-8s -\n", featureName(static_cast<FeatureId>(i)));
            out.breakdown += line;
        }
        if (s.penalty > 0.0)
        {
            char line[64];
            std::snprintf(line, sizeof(line), "penalty  -%.2f\n", s.penalty);
            out.breakdown += line;
        }
        {
            char line[64];
            std::snprintf(line, sizeof(line), "score    %.2f%s", s.score, s.wildcard ? "  (wildcard)" : "");
            out.breakdown += line;
        }
        result.suggestions.push_back(std::move(out));
    }
    result.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return result;
}

bool MatcherTriggers::update(const Snapshot& now)
{
    if (!last)
    {
        last = now;
        return now.masterDeck >= 0;
    }
    const auto& was = *last;
    const bool fire = now.masterDeck != was.masterDeck || now.trackIds != was.trackIds ||
                      (was.masterFraction < 0.75 && now.masterFraction >= 0.75) ||
                      now.masterInOutro != was.masterInOutro ||
                      now.settingsVersion != was.settingsVersion || now.analysisVersion != was.analysisVersion;
    last = now;
    return fire && now.masterDeck >= 0;
}

} // namespace devildj::matcher
