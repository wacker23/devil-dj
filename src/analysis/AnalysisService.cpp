#include "AnalysisService.h"

#include "analysis/BeatTracker.h"
#include "analysis/KeyDetector.h"
#include "analysis/FeatureNormalizer.h"
#include "analysis/Loudness.h"
#include "analysis/TrackFeatures.h"
#include "analysis/WaveformCache.h"
#include "dsp/SincResampler.h"
#include "library/Connection.h"
#include "library/Database.h"
#include "util/Log.h"
#include "util/ThreadPriority.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace devildj::analysis
{

namespace
{
std::filesystem::path fromUtf8(const std::string& s)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// The mono analysis signal: 0.5 (L + R) at 22.05 kHz. A shorter sinc than playback uses: analysis needs no
// audiophile filter, only no aliasing below ~10 kHz.
std::vector<float> monoForAnalysis(const DecodedAudio& a)
{
    std::vector<float> mono(static_cast<std::size_t>(std::max(0, a.numSamples)));
    for (int i = 0; i < a.numSamples; ++i)
        mono[static_cast<std::size_t>(i)] = 0.5f * (a.left[i] + (a.right != nullptr ? a.right[i] : a.left[i]));
    if (a.sampleRate <= 0.0 || std::abs(a.sampleRate - AnalysisInput::kMonoRate) < 1e-6)
        return mono;
    dsp::SincResamplerSettings settings;
    settings.zeroCrossings = 16;
    settings.phasesPerSample = 256;
    settings.cutoff = 0.9;
    return dsp::resample(mono, a.sampleRate, AnalysisInput::kMonoRate, settings);
}
} // namespace

std::vector<float> AnalysisService::monoSignal(const DecodedAudio& audio)
{
    return monoForAnalysis(audio);
}

void AnalysisService::beatAnalyzer(const AnalysisInput& input, AnalysisResult& result)
{
    const auto grid = trackBeats(input.mono.data(), static_cast<int>(input.mono.size()), AnalysisInput::kMonoRate);
    if (!grid.valid)
        return; // no steady beat: BPM and grid stay as they were
    result.bpm = grid.bpm;
    result.beatgridJson = beatgridJson(grid, input.full.sampleRate);
}

void AnalysisService::keyAnalyzer(const AnalysisInput& input, AnalysisResult& result)
{
    const auto key = detectKey(input.mono.data(), static_cast<int>(input.mono.size()), AnalysisInput::kMonoRate);
    if (!key.valid)
        return; // nothing tonal: a key from the tags stays
    result.musicalKey = key.key;
    result.keyConfidence = key.confidence;
}

void AnalysisService::loudnessAnalyzer(const AnalysisInput& input, AnalysisResult& result)
{
    // A mono file comes duplicated on both channels: measured as it plays, on two speakers.
    const auto& a = input.full;
    if (const auto lufs = integratedLoudness(a.left, a.right, a.numSamples, a.sampleRate))
        result.loudnessLufs = *lufs;
}

void AnalysisService::featureAnalyzer(const AnalysisInput& input, AnalysisResult& result)
{
    // The grid the beat analyzer found (in the file's samples), moved to the mono signal's rate.
    util::Beatgrid grid;
    if (result.beatgridJson && input.full.sampleRate > 0.0)
        if (const auto g = util::Beatgrid::fromJson(*result.beatgridJson, input.full.sampleRate))
            grid = g->rescaled(AnalysisInput::kMonoRate);
    const auto f = computeTrackFeatures(input.mono.data(), static_cast<int>(input.mono.size()), AnalysisInput::kMonoRate,
                                        grid);
    if (f.windows.empty())
        return;
    result.energyFeatures = packWindows(f.windows);
    if (f.sound)
        result.soundVector = packFloats(*f.sound);
    if (f.groove)
        result.groove = packFloats(*f.groove);
}

void AnalysisService::basicsAnalyzer(const AnalysisInput& input, AnalysisResult& result)
{
    // The exact length and rate from the decoded audio (tags and file headers can be off, VBR MP3s especially).
    result.durationMs = static_cast<std::int64_t>(std::llround(input.durationSeconds() * 1000.0));
    result.sampleRate = static_cast<int>(std::lround(input.full.sampleRate));
}

AnalysisService::AnalysisService(library::Database& database, Decoder decoder)
    : AnalysisService(database, std::move(decoder), Options{})
{
}

AnalysisService::AnalysisService(library::Database& database, Decoder decoder, Options o)
    : db(database)
    , decode(std::move(decoder))
    , options(std::move(o))
{
    if (options.analyzers.empty())
    {
        options.analyzers.push_back(&AnalysisService::basicsAnalyzer);
        options.analyzers.push_back(&AnalysisService::beatAnalyzer);
        options.analyzers.push_back(&AnalysisService::keyAnalyzer);
        options.analyzers.push_back(&AnalysisService::loudnessAnalyzer);
        options.analyzers.push_back(&AnalysisService::featureAnalyzer);
    }
    int n = options.workers;
    if (n <= 0)
        n = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 1, 1, 4);
    for (int i = 0; i < n; ++i)
        workers.emplace_back([this] { workerLoop(); });
}

AnalysisService::~AnalysisService()
{
    stopping = true;
    wake.notify_all();
    for (auto& w : workers)
        w.join();
    flushResults(true); // what finished is kept
    normalizeIfIdle();
}

void AnalysisService::normalizeIfIdle()
{
    {
        std::lock_guard lock(mutex);
        if (!queue.empty() || !active.empty())
            return;
    }
    {
        std::lock_guard lock(resultsMutex);
        if (!pending.empty())
            return;
    }
    if (!featuresDirty.exchange(false))
        return;
    db.writeAndWait(
        [](library::Connection& c)
        {
            std::string error;
            if (normalizeLibraryFeatures(c, &error) < 0)
                util::logWarning("Analysis: feature normalization failed: {}", error);
        });
}

void AnalysisService::enqueue(std::int64_t trackId)
{
    {
        std::lock_guard lock(mutex);
        if (!queuedOrRunning.insert(trackId).second)
            return;
        queue.push_back(trackId);
    }
    wake.notify_one();
}

void AnalysisService::prioritize(std::int64_t trackId)
{
    {
        std::lock_guard lock(mutex);
        if (queuedOrRunning.count(trackId) > 0)
        {
            const auto at = std::find(queue.begin(), queue.end(), trackId);
            if (at == queue.end())
                return; // running already
            queue.erase(at);
        }
        queuedOrRunning.insert(trackId);
        queue.push_front(trackId);
    }
    wake.notify_one();
}

int AnalysisService::queueOutdated()
{
    auto reader = db.openReader();
    if (!reader)
        return 0;
    // Failed tracks carry -version: analyzed (and failed) with this version, not retried until it changes.
    auto* s = reader.value()->prepare(
        "SELECT id FROM tracks WHERE missing = 0 AND abs(analysis_version) < ? ORDER BY date_added DESC, id");
    if (s == nullptr)
        return 0;
    s->bind(1, kAnalysisVersion);
    int n = 0;
    while (s->step())
    {
        enqueue(s->getInt(0));
        ++n;
    }
    return n;
}

AnalysisService::Status AnalysisService::status() const
{
    std::lock_guard lock(mutex);
    Status s;
    s.queued = static_cast<int>(queue.size());
    s.running = static_cast<int>(active.size());
    s.done = doneCount;
    s.failed = failedCount;
    s.active = active;
    return s;
}

bool AnalysisService::waitUntilIdle(double timeoutSeconds)
{
    std::unique_lock lock(mutex);
    const bool idle = idleSignal.wait_for(lock, std::chrono::duration<double>(timeoutSeconds),
                                          [this] { return (queue.empty() || paused) && active.empty(); });
    lock.unlock();
    flushResults(true);
    normalizeIfIdle();
    db.flush();
    return idle && status().idle();
}

void AnalysisService::setPaused(bool p)
{
    {
        std::lock_guard lock(mutex);
        paused = p;
    }
    wake.notify_all();
}

void AnalysisService::workerLoop()
{
    util::setCurrentThreadPriority(util::lowestThreadPriority());
    for (;;)
    {
        std::int64_t id = 0;
        {
            std::unique_lock lock(mutex);
            const bool woke = wake.wait_for(lock, std::chrono::milliseconds(250),
                                            [this] { return stopping.load() || (!paused && !queue.empty()); });
            if (stopping)
                return;
            if (!woke)
            {
                lock.unlock();
                flushResults(false); // results waiting for the batch timer
                normalizeIfIdle();   // P2-24: the queue is done - the library's energy percentiles again
                continue;
            }
            id = queue.front();
            queue.pop_front();
            active.emplace_back(id, 0.0);
        }
        analyze(id);
        {
            std::lock_guard lock(mutex);
            std::erase_if(active, [id](const auto& a) { return a.first == id; });
            queuedOrRunning.erase(id);
            if (queue.empty() && active.empty())
                idleSignal.notify_all();
        }
        flushResults(false);
    }
}

void AnalysisService::analyze(std::int64_t id)
{
    const auto progress = [&](double p)
    {
        std::lock_guard lock(mutex);
        for (auto& a : active)
            if (a.first == id)
                a.second = p;
    };
    const auto fail = [&]
    {
        {
            std::lock_guard lock(mutex);
            ++failedCount;
        }
        std::lock_guard lock(resultsMutex);
        pendingFailed.push_back(id);
    };

    std::string path, hash;
    {
        auto reader = db.openReader();
        if (!reader)
            return fail();
        auto* s = reader.value()->prepare("SELECT path, content_hash FROM tracks WHERE id = ?");
        if (s == nullptr || !s->bind(1, id).step())
            return; // deleted meanwhile: nothing to do
        path = s->getText(0);
        hash = s->getText(1);
        s->reset();
    }
    auto decoded = decode(fromUtf8(path), stopping);
    if (stopping)
        return;
    if (!decoded || decoded.value().numSamples <= 0 || decoded.value().left == nullptr)
        return fail();
    progress(0.5);

    AnalysisInput input{decoded.value(), monoSignal(decoded.value())};
    AnalysisResult result;
    for (std::size_t i = 0; i < options.analyzers.size(); ++i)
    {
        if (stopping)
            return;
        options.analyzers[i](input, result);
        progress(0.5 + 0.5 * static_cast<double>(i + 1) / static_cast<double>(options.analyzers.size()));
    }
    // The waveform for the decks (P2-17), at the file's rate; a deck at another rate re-labels it on load.
    const bool plainHash = !hash.empty() && std::all_of(hash.begin(), hash.end(), [](char c)
                                                         { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
    if (!options.waveformCache.empty() && plainHash)
    {
        const auto file = waveformCachePath(options.waveformCache, hash);
        std::error_code ec;
        if (!std::filesystem::exists(file, ec) && !stopping)
        {
            const auto& a = input.full;
            writeWaveformCache(file, buildWaveform(a.left, a.right != nullptr ? a.right : a.left, a.numSamples,
                                                   a.sampleRate));
        }
    }
    {
        std::lock_guard lock(mutex);
        ++doneCount;
    }
    std::lock_guard lock(resultsMutex);
    pending.emplace_back(id, std::move(result));
}

void AnalysisService::flushResults(bool force)
{
    std::vector<std::pair<std::int64_t, AnalysisResult>> results;
    std::vector<std::int64_t> failed;
    {
        std::lock_guard lock(resultsMutex);
        const auto now = std::chrono::steady_clock::now();
        const bool due = pending.size() + pendingFailed.size() >= options.batchSize ||
                         std::chrono::duration<double>(now - lastFlush).count() >= options.batchSeconds;
        if (pending.empty() && pendingFailed.empty())
            return;
        if (!force && !due)
            return;
        results.swap(pending);
        failed.swap(pendingFailed);
        lastFlush = now;
    }
    db.writeAndWait(
        [&](library::Connection& c)
        {
            c.transaction(
                [&]
                {
                    for (const auto& [id, r] : results)
                    {
                        // One UPDATE per track; BPM and grid only when the user hasn't locked them.
                        auto* s = c.prepare("UPDATE tracks SET duration_ms = coalesce(?, duration_ms), "
                                            "sample_rate = coalesce(?, sample_rate), "
                                            "bpm = CASE WHEN bpm_locked = 1 THEN bpm ELSE coalesce(?, bpm) END, "
                                            "musical_key = coalesce(?, musical_key), "
                                            "key_confidence = coalesce(?, key_confidence), "
                                            "loudness_lufs = coalesce(?, loudness_lufs), "
                                            "auto_gain_db = coalesce(?, auto_gain_db), analysis_version = ? "
                                            "WHERE id = ?");
                        if (s == nullptr)
                            return false;
                        s->bind(1, r.durationMs).bind(2, r.sampleRate).bind(3, r.bpm).bind(4, r.musicalKey);
                        s->bind(5, r.keyConfidence).bind(6, r.loudnessLufs).bind(7, r.autoGainDb);
                        s->bind(8, kAnalysisVersion).bind(9, id);
                        if (!s->run())
                            return false;
                        if (r.energyFeatures)
                        {
                            auto* f = c.prepare(
                                "INSERT INTO track_features (track_id, version, energy_features, sound_v1, groove) "
                                "VALUES (?, ?, ?, ?, ?) ON CONFLICT(track_id) DO UPDATE SET version = excluded.version, "
                                "energy_features = excluded.energy_features, sound_v1 = excluded.sound_v1, "
                                "groove = excluded.groove");
                            if (f == nullptr)
                                return false;
                            f->bind(1, id).bind(2, kFeatureVersion).bind(3, std::span<const std::uint8_t>(*r.energyFeatures));
                            r.soundVector ? f->bind(4, std::span<const std::uint8_t>(*r.soundVector)) : f->bindNull(4);
                            r.groove ? f->bind(5, std::span<const std::uint8_t>(*r.groove)) : f->bindNull(5);
                            if (!f->run())
                                return false;
                            featuresDirty = true;
                        }
                        if (r.beatgridJson)
                        {
                            auto* g = c.prepare("INSERT INTO beatgrids (track_id, anchors_json) VALUES (?, ?) "
                                                "ON CONFLICT(track_id) DO UPDATE SET anchors_json = excluded.anchors_json "
                                                "WHERE beatgrids.locked = 0");
                            if (g == nullptr || !g->bind(1, id).bind(2, *r.beatgridJson).run())
                                return false;
                        }
                        c.noteChange(library::Table::Tracks, id);
                    }
                    for (const auto id : failed)
                    {
                        auto* s = c.prepare("UPDATE tracks SET analysis_version = ? WHERE id = ?");
                        if (s == nullptr || !s->bind(1, -kAnalysisVersion).bind(2, id).run())
                            return false;
                        c.noteChange(library::Table::Tracks, id);
                    }
                    return true;
                });
        });
}

} // namespace devildj::analysis
