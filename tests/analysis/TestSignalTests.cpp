// Checks that the generated test signals decode with JUCE and match their manifest (P0-13).

#include "TestData.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <memory>

namespace
{
std::unique_ptr<juce::AudioFormatReader> openWav(const juce::File& file)
{
    juce::WavAudioFormat wav;
    return std::unique_ptr<juce::AudioFormatReader>(wav.createReaderFor(file.createInputStream().release(), true));
}
} // namespace

TEST_CASE("Generated test signals match the manifest", "[analysis][testdata]")
{
    const auto manifest = devildj::test::generatedManifest();
    if (manifest.isVoid())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");

    const auto* files = manifest["files"].getArray();
    REQUIRE(files != nullptr);
    CHECK(files->size() >= 36);

    for (const auto& entry : *files)
    {
        const auto rel = entry["file"].toString();
        INFO(rel.toStdString());
        auto reader = openWav(devildj::test::generatedFile(rel));
        REQUIRE(reader != nullptr);
        CHECK(reader->sampleRate == static_cast<double>(entry["sample_rate"]));
        CHECK(reader->numChannels == static_cast<unsigned int>(static_cast<int>(entry["channels"])));
        CHECK(reader->lengthInSamples == static_cast<juce::int64>(entry["frames"]));
    }
}

TEST_CASE("Generated click tracks start exactly at the manifest's first beat", "[analysis][testdata]")
{
    const auto manifest = devildj::test::generatedManifest();
    if (manifest.isVoid())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");

    int checked = 0;
    for (const auto& entry : *manifest["files"].getArray())
    {
        if (entry["kind"].toString() != "click")
            continue;

        const auto rel = entry["file"].toString();
        INFO(rel.toStdString());
        auto reader = openWav(devildj::test::generatedFile(rel));
        REQUIRE(reader != nullptr);

        const auto expected = static_cast<juce::int64>(entry["props"]["first_beat_frame"]);
        const int window = static_cast<int>(expected) + 1'000;
        juce::AudioBuffer<float> buffer(static_cast<int>(reader->numChannels), window);
        REQUIRE(reader->read(&buffer, 0, window, 0, true, true));

        int firstSound = -1;
        for (int i = 0; i < window && firstSound < 0; ++i)
            if (buffer.getSample(0, i) != 0.0f)
                firstSound = i;

        // The click's sine starts at zero, so the first non-zero sample is at most one frame later.
        CHECK(firstSound >= expected);
        CHECK(firstSound <= expected + 1);
        ++checked;
    }
    CHECK(checked == 6); // 5 click tracks + 1 padded
}
