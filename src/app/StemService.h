#pragma once

// Stem separation jobs (P6-03): tracks queued from the library, separated one at a time on a background thread at
// background priority (analysis::StemSeparator, HTDemucs on the CPU), the four stems written as 16-bit stereo FLAC
// at 44.1 kHz into the cache:
//
//   %LOCALAPPDATA%\Devil-DJ\cache\stems\<content hash>\{drums,bass,other,vocals}.flac + stems.json
//
// Written into "<hash>.part" and renamed when complete, so a folder that is there is whole; a cancelled or failed
// job leaves nothing. The model is the owner's own (HTDemucs's weights aren't licensed for shipping):
// Documents\Devil-DJ\Models\htdemucs_core.onnx, made by tools/htdemucs/export_onnx.py.

#include "analysis/StemSeparator.h"
#include "util/Result.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace devildj::app
{

struct StemRequest
{
    std::int64_t trackId = 0;
    juce::File file;
    std::string contentHash;
    juce::String title; // for the status line
    analysis::StemSeparator::Quality quality = analysis::StemSeparator::Quality::Fast;
};

// Decoded stereo audio (the app decodes with engine::decodeTrack; tests hand in generated audio).
struct StemAudio
{
    std::vector<float> left, right;
    double sampleRate = 44100.0;
};

class StemService
{
public:
    using Decoder = std::function<util::Result<StemAudio>(const juce::File& file)>;
    StemService(juce::File cacheRoot, juce::File model, Decoder decoder);
    ~StemService(); // stops: the track in progress is cancelled and leaves nothing

    static juce::File defaultModel(); // Documents\Devil-DJ\Models\htdemucs_core.onnx
    static const char* stemName(int index) noexcept { return analysis::demucs::kSourceNames[index]; }

    // Where a track's stems are (or would be); ready() when all four are there.
    juce::File folderFor(const std::string& contentHash) const;
    bool ready(const std::string& contentHash) const;

    void enqueue(StemRequest request); // once per track (repeats ignored); already separated tracks are skipped
    void cancelAll();

    struct Status
    {
        bool modelMissing = false;
        int queued = 0;
        std::optional<StemRequest> running;
        double progress = 0.0; // of the running one, 0..1
        int done = 0, failed = 0;
        juce::String lastError, lastDone; // the last failure's reason; the last finished track's title
        double lastSeconds = 0.0;         // how long the last one took
        bool idle() const noexcept { return queued == 0 && !running; }
    };
    Status status() const;
    bool waitUntilIdle(double timeoutSeconds) const;

private:
    struct Worker;
    void process(const StemRequest& r);
    util::Result<bool> separateInto(const StemRequest& r, const juce::File& partFolder);

    juce::File cache, model;
    Decoder decode;
    mutable std::mutex mutex;
    std::deque<StemRequest> queue;
    Status state;
    std::atomic<bool> cancelRunning{false};
    std::unique_ptr<analysis::StemSeparator> separator;
    std::unique_ptr<Worker> worker;
};

} // namespace devildj::app
