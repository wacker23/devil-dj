#pragma once

// Beat tracker v2 (P6-02): the Beat This! model (CPJKU, MIT code and weights) through ONNX Runtime, as an
// alternative to the DSP tracker (BeatTracker.h). Our own C++ port of its front-end and post-processing, checked
// against reference outputs of the original (tools/beatthis/export_onnx.py, tests/data/beatthis):
//
//   mono 22.05 kHz -> STFT (1024, hop 441 = 50 frames / s, centred, reflect-padded, periodic Hann, divided by
//   sqrt(1024)) -> magnitudes -> 128 Slaney mel bands 30 Hz .. 11 kHz -> log1p(1000 x)
//   -> the model, in chunks of 1500 frames overlapping by 2 x 6 border frames (the earlier chunk wins)
//   -> beats and downbeats: frames that are the maximum of their +-3 frames with a logit above 0, neighbours
//      merged, downbeats moved to their nearest beat
//   -> a constant beatgrid fitted to the beats (BPM, first beat, its place in the bar, how many beats it fits).

#include "analysis/BeatTracker.h"
#include "analysis/InferenceSession.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace devildj::analysis
{

namespace beatthis
{
constexpr double kSampleRate = 22050.0;
constexpr int kFft = 1024, kHop = 441, kBins = kFft / 2 + 1, kMels = 128;
constexpr double kFps = kSampleRate / kHop; // 50
constexpr int kChunk = 1500, kBorder = 6;

std::vector<float> melFilterbank();                            // kBins x kMels, row-major (torchaudio's layout)
std::vector<float> spectrogram(const float* mono, int numSamples); // frames x kMels, row-major
int frameCount(int numSamples);

struct Peaks
{
    std::vector<double> beats, downbeats; // seconds
};
Peaks peaks(const std::vector<float>& beatLogits, const std::vector<float>& downbeatLogits);

// A constant grid from beat times (and downbeats for the bar); invalid with fewer than 8 beats.
BeatGrid gridFromBeats(const std::vector<double>& beats, const std::vector<double>& downbeats);
} // namespace beatthis

class BeatThisTracker
{
public:
    static util::Result<std::unique_ptr<BeatThisTracker>> open(const std::filesystem::path& model,
                                                               InferenceOptions options = {});

    struct Output
    {
        std::vector<float> beatLogits, downbeatLogits; // one per frame
        beatthis::Peaks peaks;
        BeatGrid grid;
    };
    // Mono at 22.05 kHz. Thread-safe (ONNX Runtime sessions allow concurrent runs).
    util::Result<Output> track(const float* mono, int numSamples);
    // The logits for a spectrogram (frames x 128), chunked as the original does.
    util::Result<std::pair<std::vector<float>, std::vector<float>>> logits(const std::vector<float>& spect);

    InferenceDevice device() const noexcept { return session->device(); }

private:
    std::unique_ptr<InferenceSession> session;
};

// The grid to keep from the two trackers: the one that fits more of its own beats (within 25 ms); v1 on a tie,
// and v1 when only it found a beat (or it follows a changing tempo and v2 doesn't fit clearly better).
const BeatGrid& chooseGrid(const BeatGrid& v1, const BeatGrid& v2);

} // namespace devildj::analysis
