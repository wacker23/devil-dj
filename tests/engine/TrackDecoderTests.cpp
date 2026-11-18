// P1-04: every supported format decodes; durations within 1 ms; decoded WAV/AIFF/FLAC/ALAC equal the source.

#include "engine/TrackDecoder.h"

#include "MediaFoundationEncoder.h"
#include "TestData.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <numbers>

using devildj::engine::DecodeOptions;
using devildj::engine::decodeTrack;
using devildj::engine::TrackBuffer;

namespace
{
constexpr double kRate = 44100.0;
constexpr double kRefSeconds = 5.0;
constexpr double kLeftHz = 440.0, kRightHz = 660.0;
constexpr double kLeftAmp = 0.5, kRightAmp = 0.25;
constexpr double kOneMs = 0.001;

// The reference signal exactly as the generator wrote it (16-bit quantised).
float reference(int channel, int index)
{
    const double t = index / kRate;
    const double v = channel == 0 ? kLeftAmp * std::sin(2.0 * std::numbers::pi * kLeftHz * t)
                                  : kRightAmp * std::sin(2.0 * std::numbers::pi * kRightHz * t);
    return static_cast<float>(std::round(v * 32767.0) / 32768.0); // JUCE reads 16-bit as x / 32768
}

// MP3 and M4A go through Media Foundation, which isn't installed on every Windows (e.g. Server images).
bool needsMediaFoundation(const juce::String& name)
{
    return name.endsWithIgnoreCase(".mp3") || name.endsWithIgnoreCase(".m4a") || name.endsWithIgnoreCase(".aac");
}

bool canDecode(const juce::String& name)
{
#if JUCE_WINDOWS
    return !needsMediaFoundation(name) || devildj::test::mediaFoundationAvailable();
#else
    return !needsMediaFoundation(name);
#endif
}

juce::File formatFile(const juce::String& name)
{
    return devildj::test::generatedFile("formats/" + name);
}

bool haveSignals()
{
    return formatFile("ref.wav").existsAsFile();
}

std::unique_ptr<TrackBuffer> decodeOrFail(const juce::File& file, const DecodeOptions& options = {})
{
    auto result = decodeTrack(file, options);
    INFO(file.getFileName().toStdString() << ": " << result.error());
    REQUIRE(result.isOk());
    return std::move(result).value();
}

// Rising zero crossings per second over the middle of a channel: its pitch.
double measuredHz(const TrackBuffer& t, int channel)
{
    const float* x = t.audio.getReadPointer(channel);
    const int from = t.numSamples() / 5, to = t.numSamples() * 4 / 5;
    int crossings = 0;
    for (int i = from + 1; i < to; ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
            ++crossings;
    return crossings * t.sampleRate / (to - from);
}

double peak(const TrackBuffer& t, int channel)
{
    const auto range = t.audio.findMinMax(channel, t.numSamples() / 5, t.numSamples() * 3 / 5);
    return std::max(std::abs(range.getStart()), std::abs(range.getEnd()));
}

constexpr int kRefSamples = static_cast<int>(kRefSeconds * kRate);

// Exact match with the 16-bit reference. `trailingPadding` allows codec frame padding after the audio, which
// must be silent.
void checkLosslessMatch(const TrackBuffer& t, int trailingPadding = 0)
{
    REQUIRE(t.numSamples() >= kRefSamples);
    REQUIRE(t.numSamples() <= kRefSamples + trailingPadding);
    double maxErr = 0.0;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < kRefSamples; ++i)
            maxErr = std::max(maxErr, std::abs(static_cast<double>(t.audio.getSample(ch, i)) - reference(ch, i)));
    CHECK(maxErr <= 1.0 / 32768.0); // at most one 16-bit step (rounding differences between decoders)
    for (int ch = 0; ch < 2; ++ch)
        for (int i = kRefSamples; i < t.numSamples(); ++i)
            REQUIRE(t.audio.getSample(ch, i) == 0.0f);
}

// First and last clearly audible sample: the reference starts at sample 1 and ends at the last sample, so a
// decoder that shifts the audio (encoder delay) or adds silence shows up here.
int firstAudible(const TrackBuffer& t)
{
    for (int i = 0; i < t.numSamples(); ++i)
        if (std::abs(t.audio.getSample(0, i)) > 0.02f)
            return i;
    return -1;
}

int lastAudible(const TrackBuffer& t)
{
    for (int i = t.numSamples() - 1; i >= 0; --i)
        if (std::abs(t.audio.getSample(0, i)) > 0.02f)
            return i;
    return -1;
}

void checkPitchAndChannels(const TrackBuffer& t)
{
    CHECK(std::abs(measuredHz(t, 0) - kLeftHz) < 2.0);
    CHECK(std::abs(measuredHz(t, 1) - kRightHz) < 2.0);
    CHECK(std::abs(peak(t, 0) - kLeftAmp) < 0.05); // left and right not swapped
    CHECK(std::abs(peak(t, 1) - kRightAmp) < 0.05);
}
} // namespace

TEST_CASE("Decoder: lossless WAV, AIFF and FLAC equal the source sample for sample", "[engine][decoder]")
{
    if (!haveSignals())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");

    for (const char* name : {"ref.wav", "ref.aiff", "ref.flac"})
    {
        INFO(name);
        const auto t = decodeOrFail(formatFile(name));
        CHECK(t->sampleRate == kRate);
        CHECK(t->sourceChannels == 2);
        CHECK(std::abs(t->durationSeconds() - kRefSeconds) < kOneMs);
        checkLosslessMatch(*t);
    }
}

TEST_CASE("Decoder: lossy Ogg Vorbis and MP3 keep duration (within 1 ms), pitch and channel order", "[engine][decoder]")
{
    if (!haveSignals())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");

    for (const char* name : {"ref.ogg", "ref.mp3"})
    {
        if (!canDecode(name))
        {
            WARN("Media Foundation not available: skipping " << name);
            continue;
        }
        INFO(name);
        const auto t = decodeOrFail(formatFile(name));
        CHECK(t->sampleRate == kRate);
        INFO("duration " << t->durationSeconds() << " s");
        CHECK(std::abs(t->durationSeconds() - kRefSeconds) < kOneMs);
        CHECK(firstAudible(*t) <= 2);                              // no encoder delay left at the start
        CHECK(std::abs(lastAudible(*t) - (kRefSamples - 1)) <= 2); // no padding left at the end
        checkPitchAndChannels(*t);
    }
}

#if JUCE_WINDOWS
TEST_CASE("Decoder: AAC and ALAC in .m4a (encoded by Media Foundation for the test)", "[engine][decoder]")
{
    if (!haveSignals())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");

    const auto source = decodeOrFail(formatFile("ref.wav"));
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("ddj-m4a", {});
    dir.createDirectory();

    if (!devildj::test::mediaFoundationAvailable())
    {
        dir.deleteRecursively();
        SKIP("Media Foundation is not available on this Windows");
    }

    SECTION("ALAC is lossless")
    {
        const auto file = dir.getChildFile("ref_alac.m4a");
        const auto error = devildj::test::encodeM4a(file, source->audio, 44100, devildj::test::M4aCodec::Alac);
        if (error.isNotEmpty())
        {
            dir.deleteRecursively();
            SKIP("No ALAC encoder on this Windows: " << error.toStdString());
        }
        const auto t = decodeOrFail(file);
        CHECK(t->sampleRate == kRate);
        // The test encoder pads the last ALAC frame and declares the padded length; we match the declared
        // length to within 1 ms and the audio itself sample for sample.
        CHECK(std::abs(t->durationSeconds() - t->containerDurationSeconds) < kOneMs);
        checkLosslessMatch(*t, 4096);
    }
    SECTION("AAC keeps duration, pitch and channel order")
    {
        const auto file = dir.getChildFile("ref_aac.m4a");
        const auto error = devildj::test::encodeM4a(file, source->audio, 44100, devildj::test::M4aCodec::Aac);
        if (error.isNotEmpty())
        {
            dir.deleteRecursively();
            SKIP("No AAC encoder on this Windows: " << error.toStdString());
        }
        const auto t = decodeOrFail(file);
        CHECK(t->sampleRate == kRate);
        INFO("duration " << t->durationSeconds() << " s, declared " << t->containerDurationSeconds << " s");
        CHECK(std::abs(t->durationSeconds() - t->containerDurationSeconds) < kOneMs);
        CHECK(std::abs(lastAudible(*t) - (kRefSamples - 1)) <= 2); // priming removed: audio ends on time
        checkPitchAndChannels(*t);
    }
    dir.deleteRecursively();
}
#endif

TEST_CASE("Decoder: mono files play on both channels", "[engine][decoder]")
{
    if (!haveSignals())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");

    const auto t = decodeOrFail(formatFile("mono_440.wav"));
    CHECK(t->sourceChannels == 1);
    CHECK(t->audio.getNumChannels() == 2);
    for (int i = 0; i < t->numSamples(); i += 97)
        REQUIRE(t->audio.getSample(0, i) == t->audio.getSample(1, i));
}

TEST_CASE("Decoder: converts to the device rate on load", "[engine][decoder]")
{
    if (!haveSignals())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");

    DecodeOptions to48;
    to48.targetSampleRate = 48000.0;
    const auto t = decodeOrFail(formatFile("ref.wav"), to48);
    CHECK(t->sampleRate == 48000.0);
    CHECK(t->sourceSampleRate == kRate);
    CHECK(t->numSamples() == 240000);
    CHECK(std::abs(t->durationSeconds() - kRefSeconds) < kOneMs);
    checkPitchAndChannels(*t);

    DecodeOptions to441;
    to441.targetSampleRate = 44100.0;
    const auto down = decodeOrFail(formatFile("sine_1000hz_48k.wav"), to441);
    CHECK(down->numSamples() == 88200);
    CHECK(std::abs(measuredHz(*down, 0) - 1000.0) < 2.0);

    DecodeOptions same;
    same.targetSampleRate = kRate; // no conversion: still bit-exact
    checkLosslessMatch(*decodeOrFail(formatFile("ref.wav"), same));
}

TEST_CASE("Decoder: generated test signals all decode with the manifest's duration", "[engine][decoder]")
{
    const auto manifest = devildj::test::generatedManifest();
    if (manifest.isVoid())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");

    int decoded = 0;
    for (const auto& entry : *manifest["files"].getArray())
    {
        const auto rel = entry["file"].toString();
        if (entry["kind"].toString() == "corrupt" || !canDecode(rel))
            continue;
        INFO(rel.toStdString());
        const auto t = decodeOrFail(devildj::test::generatedFile(rel));
        CHECK(std::abs(t->durationSeconds() - static_cast<double>(entry["duration_sec"])) < kOneMs);
        ++decoded;
    }
    CHECK(decoded >= 42); // 43 audio files; the MP3 only where Media Foundation exists
}

TEST_CASE("Decoder: errors instead of crashes", "[engine][decoder]")
{
    CHECK_FALSE(decodeTrack(juce::File("C:/definitely/not/here.wav")).isOk());

    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("ddj-dec", {});
    dir.createDirectory();
    const auto text = dir.getChildFile("notes.txt");
    text.replaceWithText("hello");
    const auto unsupported = decodeTrack(text);
    CHECK_FALSE(unsupported.isOk());
    CHECK(unsupported.error().find("Unsupported") != std::string::npos);

    const auto fakeMp3 = dir.getChildFile("fake.mp3");
    fakeMp3.replaceWithText("this is not an mp3");
    CHECK_FALSE(decodeTrack(fakeMp3).isOk());

    const auto empty = dir.getChildFile("empty.flac");
    empty.create();
    CHECK_FALSE(decodeTrack(empty).isOk());
    dir.deleteRecursively();

    if (haveSignals())
    {
        const auto corrupt = decodeTrack(formatFile("corrupt.wav"));
        CHECK_FALSE(corrupt.isOk());
        CHECK_FALSE(corrupt.error().empty());
    }
}

TEST_CASE("Decoder: can be cancelled", "[engine][decoder]")
{
    if (!haveSignals())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");

    std::atomic<bool> cancel{true};
    DecodeOptions options;
    options.cancel = &cancel;
    for (const char* name : {"ref.wav", "ref.mp3"})
    {
        if (!canDecode(name))
            continue;
        const auto r = decodeTrack(formatFile(name), options);
        CHECK_FALSE(r.isOk());
        CHECK(r.error() == "Cancelled");
    }
}

TEST_CASE("Decoder: supported extensions", "[engine][decoder]")
{
    using devildj::engine::isSupportedTrackFile;
    for (const char* name : {"a.wav", "a.WAV", "a.aiff", "a.aif", "a.flac", "a.ogg"})
        CHECK(isSupportedTrackFile(juce::File::getCurrentWorkingDirectory().getChildFile(name)));
#if JUCE_WINDOWS
    for (const char* name : {"a.mp3", "a.m4a", "a.aac"})
        CHECK(isSupportedTrackFile(juce::File::getCurrentWorkingDirectory().getChildFile(name)));
#endif
    CHECK_FALSE(isSupportedTrackFile(juce::File::getCurrentWorkingDirectory().getChildFile("a.txt")));
}
