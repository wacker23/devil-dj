#pragma once

#include "util/Result.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace devildj::library
{
class Database;
}

namespace devildj::analysis
{

// Decoded audio handed to the analyzers: the file's own rate, stereo (mono files duplicated). `owner` keeps
// the samples alive (the app's decoder puts its buffer there).
struct DecodedAudio
{
    const float* left = nullptr;
    const float* right = nullptr;
    int numSamples = 0;
    double sampleRate = 0.0;
    std::shared_ptr<const void> owner;
};

// Decodes a file (the app passes engine::decodeTrack; analysis doesn't depend on the engine). Called on an
// analysis worker; `cancel` turns true when the service stops.
using Decoder = std::function<util::Result<DecodedAudio>(const std::filesystem::path& file, const std::atomic<bool>& cancel)>;

// What every analyzer gets (ARCHITECTURE §5): the full-rate stereo (waveforms) and a mono 22.05 kHz version
// (beats, key, loudness work on that).
struct AnalysisInput
{
    static constexpr double kMonoRate = 22050.0;
    const DecodedAudio& full;
    std::vector<float> mono; // 0.5 (L + R), resampled to kMonoRate
    double durationSeconds() const noexcept { return full.sampleRate > 0 ? full.numSamples / full.sampleRate : 0.0; }
};

// What the analyzers found; only set fields are written. BPM and beatgrid are left alone on tracks whose BPM is
// locked by the user.
struct AnalysisResult
{
    std::optional<std::int64_t> durationMs;
    std::optional<int> sampleRate;
    std::optional<double> bpm;
    std::optional<std::string> beatgridJson; // beatgrids.anchors_json
    std::optional<int> musicalKey;
    std::optional<double> keyConfidence;
    std::optional<double> loudnessLufs, autoGainDb;
};

// An analyzer is a pure function of the input (unit-testable with generated signals). Bump kAnalysisVersion
// when one changes, so tracks analyzed before are analyzed again.
using Analyzer = std::function<void(const AnalysisInput& input, AnalysisResult& result)>;

// The version this build's analyzers write to tracks.analysis_version. 1 = P2-12 (duration, sample rate);
// 2 = P2-13 beat tracker v1 (BPM, beatgrid); 3 = P2-15 key detection (key, confidence).
constexpr int kAnalysisVersion = 3;

// The analysis service (P2-12): a queue of track ids and a pool of workers (cores - 1, at most 4) at the
// lowest thread priority, so playing decks never wait for it. Per track: read its path from the library,
// decode (Decoder), make the mono 22.05 kHz signal, run the analyzers, collect the results and write them in
// batches (one transaction per up to 25 tracks or ~1 s) with analysis_version. Thread-safe; the destructor
// stops the workers (a track in progress is dropped, it stays queued in the library as not analyzed).
class AnalysisService
{
public:
    struct Options
    {
        int workers = 0;              // 0 = cores - 1, 1..4
        std::size_t batchSize = 25;   // results per transaction
        double batchSeconds = 1.0;    // or after this long
        std::vector<Analyzer> analyzers; // empty = the built-in ones (basicsAnalyzer)
    };

    struct Status
    {
        int queued = 0;    // waiting
        int running = 0;   // being analyzed now
        int done = 0;      // since the service started
        int failed = 0;
        std::vector<std::pair<std::int64_t, double>> active; // track id, progress 0..1
        bool idle() const noexcept { return queued == 0 && running == 0; }
    };

    AnalysisService(library::Database& db, Decoder decoder, Options options);
    AnalysisService(library::Database& db, Decoder decoder);
    ~AnalysisService();
    AnalysisService(const AnalysisService&) = delete;
    AnalysisService& operator=(const AnalysisService&) = delete;

    void enqueue(std::int64_t trackId);            // at the back (once; repeats are ignored)
    void prioritize(std::int64_t trackId);         // to the front (a track just loaded on a deck)
    int queueOutdated();                           // every present track with analysis_version < kAnalysisVersion
    Status status() const;
    bool waitUntilIdle(double timeoutSeconds);     // tests / shutdown; true when nothing is left
    void setPaused(bool paused);                   // e.g. while recording; workers finish their track and wait

    // The built-in analyzers of this version.
    static void basicsAnalyzer(const AnalysisInput& input, AnalysisResult& result);
    static void beatAnalyzer(const AnalysisInput& input, AnalysisResult& result); // P2-13
    static void keyAnalyzer(const AnalysisInput& input, AnalysisResult& result);  // P2-15

    // The analyzers' mono signal: 0.5 (L + R) resampled to AnalysisInput::kMonoRate.
    static std::vector<float> monoSignal(const DecodedAudio& audio);

private:
    void workerLoop();
    void analyze(std::int64_t trackId);
    void flushResults(bool force);

    library::Database& db;
    Decoder decode;
    Options options;
    std::atomic<bool> stopping{false};

    mutable std::mutex mutex;
    std::condition_variable wake, idleSignal;
    std::deque<std::int64_t> queue;
    std::set<std::int64_t> queuedOrRunning;
    std::vector<std::pair<std::int64_t, double>> active;
    int doneCount = 0, failedCount = 0;
    bool paused = false;

    std::mutex resultsMutex;
    std::vector<std::pair<std::int64_t, AnalysisResult>> pending;
    std::vector<std::int64_t> pendingFailed;
    std::chrono::steady_clock::time_point lastFlush = std::chrono::steady_clock::now();

    std::vector<std::thread> workers;
};

} // namespace devildj::analysis
