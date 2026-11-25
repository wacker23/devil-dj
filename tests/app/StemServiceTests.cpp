// P6-03: stem jobs - a track's four stems as FLAC in the cache, written whole or not at all.

#include "app/StemService.h"
#include "dsp/SincResampler.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <filesystem>
#include <random>

using devildj::app::StemAudio;
using devildj::app::StemRequest;
using devildj::app::StemService;
using devildj::util::Result;

namespace
{
#ifdef NDEBUG
constexpr bool kDebugBuild = false;
#else
constexpr bool kDebugBuild = true;
#endif
const juce::File kCore = juce::File(juce::String(DEVILDJ_SOURCE_DIR)).getChildFile("build/models/htdemucs_core.onnx");

struct TempFolder
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("devildj-stems", "");
    ~TempFolder() { dir.deleteRecursively(); }
};

// Kick-ish thumps, a bass line and noise hats: `seconds` of stereo at `rate`.
StemAudio music(double seconds, double rate)
{
    StemAudio a;
    a.sampleRate = rate;
    const auto n = static_cast<std::size_t>(seconds * rate);
    a.left.resize(n);
    a.right.resize(n);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    const double beat = rate * 60.0 / 124.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / rate;
        const double inBeat = std::fmod(static_cast<double>(i), beat) / rate;
        const double kick = std::exp(-inBeat * 18.0) * std::sin(2.0 * 3.14159265 * (50.0 + 90.0 * std::exp(-inBeat * 30.0)) * inBeat);
        const double bass = 0.3 * std::sin(2.0 * 3.14159265 * 55.0 * t);
        const double hat = std::fmod(inBeat + 0.5 * beat / rate, beat / rate) < 0.03 ? 0.15 * noise(rng) : 0.0;
        a.left[i] = static_cast<float>(0.5 * kick + bass + hat);
        a.right[i] = static_cast<float>(0.5 * kick + bass - hat);
    }
    return a;
}

StemRequest request(std::int64_t id, const std::string& hash)
{
    StemRequest r;
    r.trackId = id;
    r.file = juce::File("C:\\nowhere\\track.wav");
    r.contentHash = hash;
    r.title = "Track " + juce::String(id);
    return r;
}

// Reads a FLAC back: channels, rate, length and the samples.
struct Flac
{
    int channels = 0, bits = 0;
    double rate = 0.0;
    juce::AudioBuffer<float> audio;
};
std::optional<Flac> readFlac(const juce::File& f)
{
    juce::FlacAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(f.createInputStream().release(), true));
    if (reader == nullptr)
        return std::nullopt;
    Flac out;
    out.channels = static_cast<int>(reader->numChannels);
    out.bits = static_cast<int>(reader->bitsPerSample);
    out.rate = reader->sampleRate;
    out.audio.setSize(out.channels, static_cast<int>(reader->lengthInSamples));
    reader->read(&out.audio, 0, out.audio.getNumSamples(), 0, true, true);
    return out;
}
} // namespace

TEST_CASE("Stems: without the model a job fails and leaves nothing", "[app][stems]")
{
    TempFolder t;
    int decodes = 0;
    StemService service(t.dir, t.dir.getChildFile("no-model.onnx"),
                        [&](const juce::File&)
                        {
                            ++decodes;
                            return Result<StemAudio>::ok(music(2.0, 44100.0));
                        });
    CHECK(service.status().modelMissing);
    service.enqueue(request(1, "abc"));
    REQUIRE(service.waitUntilIdle(30.0));
    const auto s = service.status();
    CHECK(s.failed == 1);
    CHECK(s.done == 0);
    CHECK(s.lastError.contains("no stem model"));
    CHECK(decodes == 0); // the model is checked before the track is decoded
    CHECK(!service.ready("abc"));
    CHECK(!service.folderFor("abc").exists());
    CHECK(!t.dir.getChildFile("stems").getChildFile("abc.part").exists());
}

TEST_CASE("Stems: a track that doesn't decode fails with the reason", "[app][stems]")
{
    if (!kCore.existsAsFile())
        SKIP("No HTDemucs core: run tools/htdemucs/export_onnx.py (the owner's own weights)");
    TempFolder t;
    StemService service(t.dir, kCore, [](const juce::File&) { return Result<StemAudio>::fail("not audio"); });
    service.enqueue(request(1, "abc"));
    REQUIRE(service.waitUntilIdle(120.0));
    CHECK(service.status().failed == 1);
    CHECK(service.status().lastError.contains("not audio"));
    CHECK(!t.dir.getChildFile("stems").getChildFile("abc").exists());
    CHECK(!t.dir.getChildFile("stems").getChildFile("abc.part").exists());
}

TEST_CASE("Stems: a track becomes four 16-bit stereo FLACs at 44.1 kHz that add back up to it", "[app][stems]")
{
    // One check, so the rest isn't unreachable code in Debug (C4702).
    if (kDebugBuild || !kCore.existsAsFile())
        SKIP((kDebugBuild ? "Release only: HTDemucs on an unoptimised build takes minutes (the same code is tested in Release)"
                          : "No HTDemucs core: run tools/htdemucs/export_onnx.py (the owner's own weights)"));
    TempFolder t;
    // 48 kHz in: the service brings it to the model's 44.1 kHz.
    const auto source = music(9.0, 48000.0);
    const auto expected = music(9.0, 48000.0);
    StemService service(t.dir, kCore, [&](const juce::File&) { return Result<StemAudio>::ok(source); });
    service.enqueue(request(1, "h1"));
    service.enqueue(request(1, "h1")); // the same track again: ignored
    CHECK(service.status().queued + (service.status().running ? 1 : 0) == 1);
    REQUIRE(service.waitUntilIdle(600.0));
    const auto s = service.status();
    INFO(s.lastError);
    REQUIRE(s.done == 1);
    CHECK(s.failed == 0);
    CHECK(s.lastDone == "Track 1");
    REQUIRE(service.ready("h1"));
    CHECK(!t.dir.getChildFile("stems").getChildFile("h1.part").exists());
    CHECK(service.folderFor("h1").getChildFile("vocal.json").existsAsFile()); // P6-09: the vocal curve too
    CHECK(service.status().lastDoneTrack == 1);
    CHECK(service.status().lastDoneHash == "h1");

    const auto json = juce::JSON::parse(service.folderFor("h1").getChildFile("stems.json"));
    CHECK(json["model"].toString() == "htdemucs");
    CHECK(json["quality"].toString() == "fast");
    const auto length = static_cast<int>(std::llround(9.0 * 44100.0));
    CHECK(static_cast<int>(json["samples"]) == length);

    juce::AudioBuffer<float> sum(2, length);
    sum.clear();
    for (int stem = 0; stem < 4; ++stem)
    {
        const auto f = readFlac(service.folderFor("h1").getChildFile(juce::String(StemService::stemName(stem)) + ".flac"));
        REQUIRE(f.has_value());
        CHECK(f->channels == 2);
        CHECK(f->bits == 16);
        CHECK(f->rate == 44100.0);
        REQUIRE(f->audio.getNumSamples() == length);
        for (int c = 0; c < 2; ++c)
            sum.addFrom(c, 0, f->audio, c, 0, length);
    }
    // The stems add up to the (resampled) track, within 16-bit rounding of four files.
    juce::AudioBuffer<float> mix(2, length);
    const auto l = devildj::dsp::resample(expected.left, 48000.0, 44100.0);
    const auto r = devildj::dsp::resample(expected.right, 48000.0, 44100.0);
    REQUIRE(static_cast<int>(l.size()) == length);
    double err = 0.0, ref = 0.0;
    for (int i = 0; i < length; ++i)
    {
        const double dl = sum.getSample(0, i) - l[static_cast<std::size_t>(i)];
        const double dr = sum.getSample(1, i) - r[static_cast<std::size_t>(i)];
        err += dl * dl + dr * dr;
        ref += static_cast<double>(l[static_cast<std::size_t>(i)]) * l[static_cast<std::size_t>(i)] +
               static_cast<double>(r[static_cast<std::size_t>(i)]) * r[static_cast<std::size_t>(i)];
    }
    const double db = 10.0 * std::log10(err / ref);
    INFO("sum vs track: " << db << " dB");
    CHECK(db < -70.0);

    // Already separated: a new request does nothing.
    service.enqueue(request(1, "h1"));
    CHECK(service.status().idle());
}

TEST_CASE("Stems: cancelling stops the job and leaves no folder", "[app][stems]")
{
    // One check, so the rest isn't unreachable code in Debug (C4702).
    if (kDebugBuild || !kCore.existsAsFile())
        SKIP((kDebugBuild ? "Release only: HTDemucs on an unoptimised build takes minutes (the same code is tested in Release)"
                          : "No HTDemucs core: run tools/htdemucs/export_onnx.py (the owner's own weights)"));
    TempFolder t;
    std::atomic<bool> decoded{false};
    StemService service(t.dir, kCore,
                        [&](const juce::File&)
                        {
                            decoded = true;
                            return Result<StemAudio>::ok(music(60.0, 44100.0));
                        });
    service.enqueue(request(1, "c1"));
    service.enqueue(request(2, "c2"));
    while (!decoded)
        juce::Thread::sleep(10);
    service.cancelAll();
    REQUIRE(service.waitUntilIdle(300.0));
    const auto s = service.status();
    CHECK(s.done == 0);
    CHECK(s.failed == 0); // cancelled isn't failed
    CHECK(s.lastError.isEmpty());
    CHECK(!service.ready("c1"));
    CHECK(!service.ready("c2"));
    CHECK(t.dir.getChildFile("stems").findChildFiles(juce::File::findFilesAndDirectories, false).isEmpty());
}
