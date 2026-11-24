// P6-03: stem separation with HTDemucs - the STFT port, one segment against PyTorch, whole tracks streamed.

#include "analysis/StemSeparator.h"
#include "engine/TrackDecoder.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>

using namespace devildj::analysis;

namespace
{
#ifdef NDEBUG
constexpr bool kDebugBuild = false;
#else
constexpr bool kDebugBuild = true;
#endif
const std::filesystem::path kModels = std::filesystem::path(DEVILDJ_SOURCE_DIR) / "build" / "models";
const std::filesystem::path kCore = kModels / "htdemucs_core.onnx";

std::vector<float> readFloats(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::vector<float> v;
    float x = 0.0f;
    while (in.read(reinterpret_cast<char*>(&x), sizeof x))
        v.push_back(x);
    return v;
}

// Error of mix - sum(stems) against the mix, in dB.
double sumBackErrorDb(const std::vector<float>& l, const std::vector<float>& r, const std::array<std::vector<float>, 8>& stems)
{
    double err = 0.0, ref = 0.0;
    for (std::size_t i = 0; i < l.size(); ++i)
        for (int c = 0; c < 2; ++c)
        {
            const double x = c == 0 ? l[i] : r[i];
            double s = 0.0;
            for (int src = 0; src < 4; ++src)
                s += stems[static_cast<std::size_t>(src * 2 + c)][i];
            err += (x - s) * (x - s);
            ref += x * x;
        }
    return 10.0 * std::log10(std::max(1e-30, err) / std::max(1e-30, ref));
}

std::array<std::vector<float>, 8> collect(StemSeparator& s, const std::vector<float>& l, const std::vector<float>& r,
                                          StemSeparator::Quality q, double& seconds)
{
    std::array<std::vector<float>, 8> stems;
    const auto t0 = std::chrono::steady_clock::now();
    const auto ok = s.separate(l.data(), r.data(), static_cast<std::int64_t>(l.size()), q,
                               [&](const float* const* ch, int n)
                               {
                                   for (int c = 0; c < 8; ++c)
                                       stems[static_cast<std::size_t>(c)].insert(stems[static_cast<std::size_t>(c)].end(), ch[c], ch[c] + n);
                               });
    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    INFO((ok ? std::string() : ok.error()));
    REQUIRE(ok);
    return stems;
}
} // namespace

TEST_CASE("Stems: demucs's STFT and inverse STFT give the segment back", "[analysis][stems]")
{
    std::mt19937 rng(5);
    std::normal_distribution<float> noise(0.0f, 0.3f);
    std::vector<float> l(demucs::kSegment), r(demucs::kSegment);
    for (int i = 0; i < demucs::kSegment; ++i)
    {
        l[static_cast<std::size_t>(i)] = noise(rng) + 0.5f * std::sin(i * 0.01f);
        r[static_cast<std::size_t>(i)] = noise(rng);
    }
    const auto spec = demucs::spectrogram(l.data(), r.data());
    REQUIRE(spec.size() == static_cast<std::size_t>(4) * demucs::kBins * demucs::kFrames);
    std::vector<float> bl(demucs::kSegment), br(demucs::kSegment);
    demucs::inverse(spec.data(), demucs::kSegment, bl.data(), br.data());
    // The middle: demucs drops the STFT's first and last two frames, so the segment's first and last ~3000 samples
    // can't come back exactly (the overlapping, triangle-weighted segments of a track hide that).
    double worst = 0.0;
    for (int i = 4096; i < demucs::kSegment - 4096; ++i)
        worst = std::max({worst, std::abs(static_cast<double>(bl[static_cast<std::size_t>(i)]) - l[static_cast<std::size_t>(i)]),
                          std::abs(static_cast<double>(br[static_cast<std::size_t>(i)]) - r[static_cast<std::size_t>(i)])});
    WARN("STFT round trip: worst error " << worst);
    // Only the top bin (dropped, as demucs drops it) is lost: noise has a little energy there.
    CHECK(worst < 0.05);
}

TEST_CASE("Stems: one segment through the model matches PyTorch (needs build/models)", "[analysis][stems][model]")
{
    if (!std::filesystem::exists(kCore))
        SKIP("No HTDemucs core: run tools/htdemucs/export_onnx.py (the owner's own weights)");
    InferenceOptions o;
    o.preferGpu = false;
    auto s = StemSeparator::open(kCore, o);
    INFO((s ? std::string() : s.error()));
    REQUIRE(s);
    const auto in = readFloats(kModels / "htdemucs_ref_in.f32");
    const auto ref = readFloats(kModels / "htdemucs_ref_out.f32");
    REQUIRE(in.size() == static_cast<std::size_t>(2) * demucs::kSegment);
    const auto out = s.value()->separateSegment(in.data(), in.data() + demucs::kSegment);
    INFO((out ? std::string() : out.error()));
    REQUIRE(out);
    REQUIRE(out.value().size() == ref.size());
    double worst = 0.0, peak = 0.0;
    for (std::size_t i = 0; i < ref.size(); ++i)
    {
        worst = std::max(worst, static_cast<double>(std::abs(out.value()[i] - ref[i])));
        peak = std::max(peak, static_cast<double>(std::abs(ref[i])));
    }
    WARN("segment vs PyTorch: worst |diff| " << worst << " (peak " << peak << ")");
    CHECK(worst < 2e-3);
    // DirectML gets this network wrong (ORT 1.24): the separator runs it on the CPU. Logged here so a fixed
    // runtime shows up.
    CHECK(s.value()->device() == InferenceDevice::Cpu);
    InferenceOptions gpu;
    if (auto g = InferenceSession::open(kCore, gpu); g && g.value()->device() == InferenceDevice::DirectML)
    {
        Tensor mag{{1, 4, demucs::kBins, demucs::kFrames}, demucs::spectrogram(in.data(), in.data() + demucs::kSegment)};
        Tensor mix{{1, 2, demucs::kSegment}, in};
        const auto raw = g.value()->run({mag, mix});
        const auto cpu = InferenceSession::open(kCore, o).value()->run({mag, mix});
        if (raw && cpu)
        {
            double gw = 0.0;
            for (std::size_t i = 0; i < cpu.value()[1].data.size(); ++i)
                gw = std::max(gw, static_cast<double>(std::abs(raw.value()[1].data[i] - cpu.value()[1].data[i])));
            WARN("the core on DirectML vs the CPU: worst |diff| " << gw << (gw < 1e-2 ? " - DirectML works now" : " - still wrong"));
        }
    }
}

TEST_CASE("Stems: a 25 s piece streamed - every sample, the stems sum back to the mix (needs build/models)", "[analysis][stems][model]")
{
    // One check, so the rest isn't unreachable code in Debug (C4702).
    if (kDebugBuild || !std::filesystem::exists(kCore))
        SKIP((kDebugBuild ? "Release only: HTDemucs on an unoptimised build takes many minutes (the same code is tested in Release)"
                          : "No HTDemucs core: run tools/htdemucs/export_onnx.py (the owner's own weights)"));
    auto s = StemSeparator::open(kCore);
    REQUIRE(s);
    // 25 s of the reference segment's music, looped (so segments overlap real audio on both sides).
    const auto seg = readFloats(kModels / "htdemucs_ref_in.f32");
    const int n = static_cast<int>(25 * demucs::kSampleRate);
    std::vector<float> l(static_cast<std::size_t>(n)), r(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        l[static_cast<std::size_t>(i)] = seg[static_cast<std::size_t>(i % demucs::kSegment)];
        r[static_cast<std::size_t>(i)] = seg[static_cast<std::size_t>(demucs::kSegment + i % demucs::kSegment)];
    }
    double seconds = 0.0;
    const auto stems = collect(*s.value(), l, r, StemSeparator::Quality::Fast, seconds);
    for (const auto& c : stems)
        REQUIRE(c.size() == l.size());
    const double err = sumBackErrorDb(l, r, stems);
    WARN("25 s on " << (s.value()->device() == InferenceDevice::DirectML ? "DirectML" : "the CPU") << " in " << seconds
                    << " s (fast); stems sum back within " << err << " dB");
    CHECK(err < -60.0); // other is the remainder: exact but for float rounding
    // Quality: three passes a segment, a little closer if anything.
    double qs = 0.0;
    const auto q = collect(*s.value(), l, r, StemSeparator::Quality::Quality, qs);
    const double qerr = sumBackErrorDb(l, r, q);
    WARN("quality in " << qs << " s; within " << qerr << " dB");
    CHECK(qerr < -60.0);
    // Cancelling stops it.
    const auto cancelled = s.value()->separate(l.data(), r.data(), n, StemSeparator::Quality::Fast, [](const float* const*, int) {},
                                               [](double) { return false; });
    CHECK_FALSE(cancelled);
}

// The done check (P6-03): a track of 5 minutes or more from the owner's evaluation folder, timed on this laptop.
//   $env:DEVILDJ_EVAL_DIR = "C:\Users\sophi\Music\DJ_collection"; DevilDJTests.exe "[.stemseval]"
TEST_CASE("Stems: a 5-minute track from the owner's folder (timing logged)", "[analysis][stems][.stemseval]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable("DEVILDJ_EVAL_DIR", {});
    if (dir.isEmpty() || !std::filesystem::exists(kCore))
        SKIP("Set DEVILDJ_EVAL_DIR and export the HTDemucs core");
    auto s = StemSeparator::open(kCore);
    REQUIRE(s);
    for (const auto& file : juce::File(dir).findChildFiles(juce::File::findFiles, true))
    {
        if (!devildj::engine::isSupportedTrackFile(file))
            continue;
        auto decoded = devildj::engine::decodeTrack(file);
        if (!decoded || decoded.value()->audio.getNumSamples() < 5 * 60 * decoded.value()->sampleRate)
            continue;
        if (std::abs(decoded.value()->sampleRate - demucs::kSampleRate) > 1.0)
            continue; // 44.1 kHz here: resampling is the job's (tested elsewhere)
        const auto& b = decoded.value()->audio;
        std::vector<float> l(b.getReadPointer(0), b.getReadPointer(0) + b.getNumSamples());
        std::vector<float> r(b.getReadPointer(b.getNumChannels() > 1 ? 1 : 0), b.getReadPointer(b.getNumChannels() > 1 ? 1 : 0) + b.getNumSamples());
        double seconds = 0.0;
        const auto stems = collect(*s.value(), l, r, StemSeparator::Quality::Fast, seconds);
        const double err = sumBackErrorDb(l, r, stems);
        const double minutes = b.getNumSamples() / demucs::kSampleRate / 60.0;
        WARN(file.getFileName() << ": " << minutes << " min on "
                                << (s.value()->device() == InferenceDevice::DirectML ? "DirectML" : "the CPU") << " in " << seconds
                                << " s (" << seconds / (minutes * 60.0) << " x real time); stems sum back within " << err << " dB");
        CHECK(err < -60.0); // other is the remainder: exact but for float rounding
        return; // one is the check
    }
    SKIP("No track of 5 minutes or more at 44.1 kHz in the folder");
}
