// P6-02: beat tracker v2 (Beat This!) - our C++ port checked against the original's reference outputs.

#include "analysis/AnalysisService.h"
#include "analysis/BeatThis.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace devildj::analysis;

namespace
{
const std::filesystem::path kData = std::filesystem::path(DEVILDJ_SOURCE_DIR) / "tests" / "data" / "beatthis";
const std::filesystem::path kModel = std::filesystem::path(DEVILDJ_SOURCE_DIR) / "build" / "models" / "beat_this_final0.onnx";

std::vector<float> readFloats(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::vector<float> v;
    float x = 0.0f;
    while (in.read(reinterpret_cast<char*>(&x), sizeof x))
        v.push_back(x);
    return v;
}

std::vector<float> readClip()
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> r(
        wav.createReaderFor(new juce::FileInputStream(juce::File(juce::String((kData / "clip.wav").wstring().c_str()))), true));
    REQUIRE(r != nullptr);
    REQUIRE(r->sampleRate == 22050.0);
    juce::AudioBuffer<float> b(1, static_cast<int>(r->lengthInSamples));
    r->read(&b, 0, b.getNumSamples(), 0, true, false);
    return {b.getReadPointer(0), b.getReadPointer(0) + b.getNumSamples()};
}

struct RefBeats
{
    std::vector<double> beats, downbeats;
};
RefBeats readBeats()
{
    RefBeats r;
    std::ifstream in(kData / "beats.txt");
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream s(line);
        double t = 0.0;
        int down = 0;
        s >> t >> down;
        r.beats.push_back(t);
        if (down == 1)
            r.downbeats.push_back(t);
    }
    return r;
}

double maxDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    double m = 0.0;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        m = std::max(m, static_cast<double>(std::abs(a[i] - b[i])));
    return m;
}
} // namespace

TEST_CASE("Beat This! front-end: the mel filterbank is torchaudio's", "[analysis][beatthis]")
{
    const auto ref = readFloats(kData / "mel_fbank.f32");
    const auto fb = beatthis::melFilterbank();
    REQUIRE(ref.size() == fb.size());
    CHECK(maxDiff(fb, ref) < 1e-4); // torchaudio computes it in single precision
}

TEST_CASE("Beat This! front-end: the spectrogram matches the original's for the reference clip", "[analysis][beatthis]")
{
    const auto clip = readClip();
    const auto ref = readFloats(kData / "spect.f32");
    const auto spect = beatthis::spectrogram(clip.data(), static_cast<int>(clip.size()));
    CHECK(beatthis::frameCount(static_cast<int>(clip.size())) == 401);
    REQUIRE(spect.size() == ref.size());
    const double d = maxDiff(spect, ref);
    WARN("spectrogram vs the original: max |diff| " << d << " (values up to ~10)");
    CHECK(d < 2e-3);
}

TEST_CASE("Beat This! post-processing: the reference logits give the reference beats", "[analysis][beatthis]")
{
    const auto logits = readFloats(kData / "logits.f32");
    std::vector<float> beat, down;
    for (std::size_t i = 0; i + 1 < logits.size(); i += 2)
    {
        beat.push_back(logits[i]);
        down.push_back(logits[i + 1]);
    }
    const auto p = beatthis::peaks(beat, down);
    const auto ref = readBeats();
    REQUIRE(p.beats.size() == ref.beats.size());
    for (std::size_t i = 0; i < ref.beats.size(); ++i)
        CHECK(std::abs(p.beats[i] - ref.beats[i]) < 1e-4);
    REQUIRE(p.downbeats.size() == ref.downbeats.size());
    for (std::size_t i = 0; i < ref.downbeats.size(); ++i)
        CHECK(std::abs(p.downbeats[i] - ref.downbeats[i]) < 1e-4);
    // Edges: nothing above 0 is no beat; a plateau of two equal frames is one beat between them.
    CHECK(beatthis::peaks(std::vector<float>(50, -1.0f), std::vector<float>(50, -1.0f)).beats.empty());
    std::vector<float> plateau(50, -5.0f);
    plateau[20] = plateau[21] = 3.0f;
    const auto pl = beatthis::peaks(plateau, std::vector<float>(50, -5.0f));
    REQUIRE(pl.beats.size() == 1);
    CHECK(std::abs(pl.beats[0] - 20.5 / 50.0) < 1e-9);
}

TEST_CASE("Beat This! grid: a constant grid from beats, its bar from the downbeats", "[analysis][beatthis]")
{
    // 126 BPM from 0.731 s, 64 beats with +-8 ms of jitter, two missing and one stray; downbeats on beats 2, 6, 10 ...
    std::vector<double> beats, downs;
    const double period = 60.0 / 126.0;
    for (int k = 0; k < 64; ++k)
    {
        if (k == 17 || k == 40)
            continue;
        const double t = 0.731 + k * period + 0.008 * std::sin(k * 1.7);
        beats.push_back(t);
        if (k % 4 == 2)
            downs.push_back(t);
    }
    beats.push_back(0.731 + 30.5 * period); // a stray off-beat
    std::sort(beats.begin(), beats.end());
    const auto g = beatthis::gridFromBeats(beats, downs);
    REQUIRE(g.valid);
    CHECK(std::abs(g.bpm - 126.0) < 0.05);
    CHECK(std::abs(g.firstBeatSeconds - 0.731) < 0.01);
    CHECK(g.firstBeatInBar == 3); // beat 0 is two before a downbeat: the 3rd of its bar
    CHECK(g.beatsUsed == 62);
    CHECK(g.constantHitRate > 0.95);
    CHECK_FALSE(beatthis::gridFromBeats({1.0, 1.5, 2.0}, {}).valid); // too few
}

TEST_CASE("Beat This! choosing between v1 and v2 by how well each fits its beats", "[analysis][beatthis]")
{
    BeatGrid v1, v2;
    v1.valid = v2.valid = true;
    v1.constantHitRate = 0.8;
    v2.constantHitRate = 0.9;
    CHECK(&chooseGrid(v1, v2) == &v2);
    v2.constantHitRate = 0.8;
    CHECK(&chooseGrid(v1, v2) == &v1); // a tie: v1
    v2.valid = false;
    CHECK(&chooseGrid(v1, v2) == &v1);
    v2.valid = true;
    v1.valid = false;
    CHECK(&chooseGrid(v1, v2) == &v2);
    v1.valid = true;
    v1.variableTempo = true;
    v1.followHitRate = 0.9;
    v2.constantHitRate = 0.95;
    CHECK(&chooseGrid(v1, v2) == &v1); // a live band: the following grid unless v2 is clearly better
    v2.constantHitRate = 1.01;
    CHECK(&chooseGrid(v1, v2) == &v2);
}

TEST_CASE("Beat This! model: the logits and beats match the original's (needs build/models)", "[analysis][beatthis][model]")
{
    if (!std::filesystem::exists(kModel))
        SKIP("No exported model: run tools/beatthis/export_onnx.py");
    InferenceOptions o;
    o.preferGpu = false; // the reference was made on the CPU
    auto t = BeatThisTracker::open(kModel, o);
    INFO((t ? std::string() : t.error()));
    REQUIRE(t);
    // The model on the reference spectrogram.
    const auto spect = readFloats(kData / "spect.f32");
    const auto l = t.value()->logits(spect);
    REQUIRE(l);
    const auto ref = readFloats(kData / "logits.f32");
    std::vector<float> refBeat, refDown;
    for (std::size_t i = 0; i + 1 < ref.size(); i += 2)
    {
        refBeat.push_back(ref[i]);
        refDown.push_back(ref[i + 1]);
    }
    const double db = maxDiff(l.value().first, refBeat), dd = maxDiff(l.value().second, refDown);
    WARN("logits vs the original: beat " << db << ", downbeat " << dd);
    CHECK(db < 1e-3);
    CHECK(dd < 1e-3);
    // The whole path from audio: the same beats.
    const auto clip = readClip();
    const auto out = t.value()->track(clip.data(), static_cast<int>(clip.size()));
    REQUIRE(out);
    const auto beats = readBeats();
    REQUIRE(out.value().peaks.beats.size() == beats.beats.size());
    for (std::size_t i = 0; i < beats.beats.size(); ++i)
        CHECK(std::abs(out.value().peaks.beats[i] - beats.beats[i]) < 0.021); // within one frame
    CHECK(out.value().grid.valid);
    CHECK(std::abs(out.value().grid.bpm - 124.0) < 0.5);
}

TEST_CASE("Beat This! model: a long piece in chunks - every frame predicted, the beat steady across chunk lines", "[analysis][beatthis][model]")
{
    if (!std::filesystem::exists(kModel))
        SKIP("No exported model: run tools/beatthis/export_onnx.py");
    InferenceOptions o;
    o.preferGpu = false; // compared with the original's CPU run
    auto t = BeatThisTracker::open(kModel, o);
    REQUIRE(t);
    // The clip is 124 BPM; its first 16 beats (170,710 samples, off by 0.3 of a sample) end to end keep the beat.
    const auto clip = readClip();
    const auto bars = static_cast<std::ptrdiff_t>(std::llround(16.0 * 60.0 / 124.0 * 22050.0));
    std::vector<float> longPiece;
    while (longPiece.size() < static_cast<std::size_t>(70 * 22050))
        longPiece.insert(longPiece.end(), clip.begin(), clip.begin() + bars);
    const auto out = t.value()->track(longPiece.data(), static_cast<int>(longPiece.size()));
    REQUIRE(out);
    CHECK(out.value().beatLogits.size() == static_cast<std::size_t>(beatthis::frameCount(static_cast<int>(longPiece.size()))));
    CHECK(std::none_of(out.value().beatLogits.begin(), out.value().beatLogits.end(), [](float v) { return v <= -999.0f; }));
    // The chunked logits are the original's chunked logits (tools/beatthis/export_onnx.py: long_logits.f32).
    const auto ref = readFloats(kData / "long_logits.f32");
    std::vector<float> refBeat, refDown;
    for (std::size_t i = 0; i + 1 < ref.size(); i += 2)
    {
        refBeat.push_back(ref[i]);
        refDown.push_back(ref[i + 1]);
    }
    REQUIRE(refBeat.size() == out.value().beatLogits.size());
    const double db = maxDiff(out.value().beatLogits, refBeat), dd = maxDiff(out.value().downbeatLogits, refDown);
    WARN("70 s chunked logits vs the original: beat " << db << ", downbeat " << dd);
    CHECK(db < 0.05);
    CHECK(dd < 0.05);
    const auto& g = out.value().grid;
    WARN("70 s, " << out.value().peaks.beats.size() << " beats on " << (t.value()->device() == InferenceDevice::DirectML ? "DirectML" : "the CPU")
                  << ": " << g.bpm << " BPM, " << 100.0 * g.constantHitRate << " % on the grid");
    REQUIRE(g.valid);
    CHECK(std::abs(g.bpm - 124.0) < 0.3);
    CHECK(g.constantHitRate > 0.9);
}

TEST_CASE("Beat This! in the analysis service: both trackers, the better grid kept (needs build/models)", "[analysis][beatthis][model]")
{
    if (!std::filesystem::exists(kModel))
        SKIP("No exported model: run tools/beatthis/export_onnx.py");
    auto t = BeatThisTracker::open(kModel);
    REQUIRE(t);
    std::shared_ptr<BeatThisTracker> v2(std::move(t.value()));
    // 30 s of clicks at 120 BPM (a short decaying burst on every beat, louder on the downbeats).
    const int n = 30 * 22050;
    std::vector<float> mono(static_cast<std::size_t>(n), 0.0f);
    for (int k = 0; k * 0.5 < 30.0; ++k)
    {
        const int at = static_cast<int>(k * 0.5 * 22050);
        for (int i = 0; i < 400 && at + i < n; ++i)
            mono[static_cast<std::size_t>(at + i)] = static_cast<float>((k % 4 == 0 ? 0.9 : 0.6) * std::sin(i * 0.3) * std::exp(-i / 80.0));
    }
    const DecodedAudio full{mono.data(), mono.data(), n, 22050.0, {}};
    for (const bool withV2 : {true, false})
    {
        INFO((withV2 ? "with v2" : "v1 alone"));
        AnalysisInput input{full, mono};
        AnalysisResult result;
        AnalysisService::beatAnalyzerWith(withV2 ? v2 : nullptr)(input, result);
        REQUIRE(result.bpm.has_value());
        CHECK(std::abs(*result.bpm - 120.0) < 0.2);
        CHECK(result.beatgridJson.has_value());
    }
}
