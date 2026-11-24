#pragma once

// Stem separation (P6-03): a track into drums, bass, other and vocals with HTDemucs (Demucs v4; MIT code - the
// weights' licence is not clear, so the owner provides them for their own use and they're never shipped).
//
// The network core runs through ONNX Runtime (tools/htdemucs/export_onnx.py exports it); its STFT and inverse STFT -
// complex numbers, which don't export - are done here exactly as demucs.htdemucs does them (n_fft 4096, hop 1024,
// normalised, demucs's reflect padding, 2048 bins, the 2-frame margins dropped). Long tracks go in 7.8 s segments
// as demucs.apply does: the track normalised by its mono mean and deviation, segments a quarter overlapping with
// triangular weights, each padded from the audio around it (zeros past the ends).
//
// "other" is the remainder (the mix less drums, bass and vocals), so the stems always add up to the track exactly.
//
// Streaming: only a rolling window of the output is kept; each stretch of samples goes to the sink as soon as no
// later segment overlaps it, so memory doesn't grow with the track's length. "Quality" also runs every segment
// shifted by a quarter and three quarters of half a second (demucs's shift trick, per segment) and averages.
//
// Analysis threads only (never the audio thread); 44.1 kHz stereo in and out. Always on the CPU: DirectML (ORT 1.24)
// gets this network wrong (see StemSeparator::open).

#include "analysis/InferenceSession.h"
#include "util/Result.h"

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace devildj::analysis
{

namespace demucs
{
constexpr double kSampleRate = 44100.0;
constexpr int kSegment = 343980; // 7.8 s
constexpr int kFft = 4096, kHop = 1024, kBins = 2048, kFrames = 336;
constexpr int kSources = 4;
constexpr const char* kSourceNames[kSources] = {"drums", "bass", "other", "vocals"};

// The STFT of a segment as the model takes it: [4][kBins][kFrames], channels L.re L.im R.re R.im.
std::vector<float> spectrogram(const float* left, const float* right);
// One source's two channels back from that layout ([4][kBins][kFrames] for it), `length` samples each.
void inverse(const float* spec, int length, float* left, float* right);
} // namespace demucs

class StemSeparator
{
public:
    enum class Quality
    {
        Fast,    // one pass per segment
        Quality, // and two shifted ones, averaged (about 3x the time)
    };

    static util::Result<std::unique_ptr<StemSeparator>> open(const std::filesystem::path& model, InferenceOptions options = {});

    // One 7.8 s segment (exactly demucs::kSegment samples a channel) through the model:
    // out[source][channel] (kSources x 2 x kSegment, row-major).
    util::Result<std::vector<float>> separateSegment(const float* left, const float* right);

    // A whole track at 44.1 kHz. The sink gets the stems in order, a stretch at a time: channels[source * 2 + c]
    // (8 pointers), `count` samples. `progress` (0..1) returns false to cancel (the result is then an error).
    using Sink = std::function<void(const float* const* channels, int count)>;
    util::Result<bool> separate(const float* left, const float* right, std::int64_t numSamples, Quality quality,
                                const Sink& sink, const std::function<bool(double)>& progress = {});

    InferenceDevice device() const noexcept { return session->device(); }

private:
    std::unique_ptr<InferenceSession> session;
};

} // namespace devildj::analysis
