// P2-16: loudness (ITU-R BS.1770-4 integrated loudness) and Auto Gain. Done when: test vectors (generated sine at
// known level) within 0.1 LU.

#include "TestData.h"
#include "analysis/AnalysisService.h"
#include "analysis/Loudness.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <vector>

using namespace devildj;

namespace
{
// A sine whose peak is `dbfs` (a stereo 1 kHz sine at -23 dBFS measures -23 LUFS).
std::vector<float> sine(double hz, double dbfs, double seconds, double rate)
{
    std::vector<float> x(static_cast<std::size_t>(seconds * rate));
    const double a = std::pow(10.0, dbfs / 20.0);
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(a * std::sin(2.0 * 3.141592653589793 * hz * static_cast<double>(i) / rate));
    return x;
}

std::vector<float> join(std::initializer_list<std::vector<float>> parts)
{
    std::vector<float> out;
    for (const auto& p : parts)
        out.insert(out.end(), p.begin(), p.end());
    return out;
}

std::optional<double> stereo(const std::vector<float>& x, double rate)
{
    return analysis::integratedLoudness(x.data(), x.data(), static_cast<int>(x.size()), rate);
}
} // namespace

TEST_CASE("Loudness: the K-weighting filters equal BS.1770's 48 kHz coefficients", "[analysis][loudness]")
{
    const auto k = analysis::kWeighting(48000.0);
    // As printed in ITU-R BS.1770-4, tables 1 and 2.
    CHECK(std::abs(k[0].b0 - 1.53512485958697) < 1e-9);
    CHECK(std::abs(k[0].b1 - -2.69169618940638) < 1e-9);
    CHECK(std::abs(k[0].b2 - 1.19839281085285) < 1e-9);
    CHECK(std::abs(k[0].a1 - -1.69065929318241) < 1e-9);
    CHECK(std::abs(k[0].a2 - 0.73248077421585) < 1e-9);
    CHECK(k[1].b0 == 1.0);
    CHECK(k[1].b1 == -2.0);
    CHECK(k[1].b2 == 1.0);
    CHECK(std::abs(k[1].a1 - -1.99004745483398) < 1e-9);
    CHECK(std::abs(k[1].a2 - 0.99007225036621) < 1e-9);
}

TEST_CASE("Loudness: generated sines at known levels, within 0.1 LU", "[analysis][loudness]")
{
    // The generated test files: 1 kHz stereo sines at -20 and -6 dBFS (44.1 kHz, 16 bit).
    const auto manifest = test::generatedManifest();
    if (!manifest.isObject())
        SKIP("Generated test signals missing (python tools/gen_test_signals.py)");
    int checked = 0;
    for (const auto& entry : *manifest["files"].getArray())
    {
        if (entry["kind"].toString() != "sine" || static_cast<double>(entry["props"]["frequency_hz"]) != 1000.0)
            continue;
        const double level = entry["props"]["level_dbfs"];
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(
            formats.createReaderFor(test::generatedFile(entry["file"].toString())));
        REQUIRE(reader != nullptr);
        juce::AudioBuffer<float> buffer(2, static_cast<int>(reader->lengthInSamples));
        reader->read(&buffer, 0, buffer.getNumSamples(), 0, true, true);
        const auto lufs = analysis::integratedLoudness(buffer.getReadPointer(0), buffer.getReadPointer(1),
                                                       buffer.getNumSamples(), reader->sampleRate);
        INFO(entry["file"].toString() << ": " << (lufs ? *lufs : -999.0) << " LUFS");
        REQUIRE(lufs.has_value());
        CHECK(std::abs(*lufs - level) < 0.1);
        WARN(entry["file"].toString() << ": " << *lufs << " LUFS (error " << *lufs - level << " LU)");
        ++checked;
    }
    CHECK(checked == 2);

    // Made here, at the usual rates and levels; a mono signal (one channel) is 3 dB below the same in stereo.
    for (const double rate : {44100.0, 48000.0, 96000.0})
        for (const double level : {-23.0, -33.0, -10.0})
        {
            INFO(rate << " Hz, " << level << " dBFS");
            const auto x = sine(1000.0, level, 20.0, rate);
            const auto s = stereo(x, rate);
            REQUIRE(s.has_value());
            CHECK(std::abs(*s - level) < 0.1);
            const auto m = analysis::integratedLoudness(x.data(), nullptr, static_cast<int>(x.size()), rate);
            REQUIRE(m.has_value());
            CHECK(std::abs(*m - (level - 3.0103)) < 0.1);
        }
}

TEST_CASE("Loudness: the gates", "[analysis][loudness]")
{
    constexpr double rate = 48000.0;
    // Relative gate: quiet passages more than 10 LU down don't count (-36 / -23 / -36 dBFS -> -23 LUFS).
    {
        const auto x = join({sine(1000.0, -36.0, 10.0, rate), sine(1000.0, -23.0, 20.0, rate), sine(1000.0, -36.0, 10.0, rate)});
        const auto l = stereo(x, rate);
        REQUIRE(l.has_value());
        CHECK(std::abs(*l - -23.0) < 0.1);
    }
    // ...but closer ones do: -26 / -20 dBFS halves average their energy.
    {
        const auto x = join({sine(1000.0, -26.0, 10.0, rate), sine(1000.0, -20.0, 10.0, rate)});
        const auto l = stereo(x, rate);
        REQUIRE(l.has_value());
        const double expected = 10.0 * std::log10(0.5 * (std::pow(10.0, -2.6) + std::pow(10.0, -2.0)));
        CHECK(std::abs(*l - expected) < 0.1);
    }
    // Absolute gate: silence (and anything under -70) is left out.
    {
        const auto x = join({std::vector<float>(static_cast<std::size_t>(10 * rate), 0.0f), sine(1000.0, -23.0, 20.0, rate),
                             sine(1000.0, -75.0, 10.0, rate)});
        const auto l = stereo(x, rate);
        REQUIRE(l.has_value());
        CHECK(std::abs(*l - -23.0) < 0.1);
    }
    // No loudness: silence, only under -70, shorter than one 400 ms block.
    CHECK_FALSE(stereo(std::vector<float>(static_cast<std::size_t>(5 * rate), 0.0f), rate).has_value());
    CHECK_FALSE(stereo(sine(1000.0, -80.0, 5.0, rate), rate).has_value());
    CHECK_FALSE(stereo(sine(1000.0, -10.0, 0.3, rate), rate).has_value());
    // The K-weighting: low bass counts much less than the same level at 1 kHz, the top a little more.
    const auto low = stereo(sine(30.0, -20.0, 10.0, rate), rate);
    const auto high = stereo(sine(8000.0, -20.0, 10.0, rate), rate);
    REQUIRE(low.has_value());
    REQUIRE(high.has_value());
    CHECK(*low < -20.0 - 3.0);
    CHECK(*high > -20.0 + 3.0);
}

TEST_CASE("Auto Gain: the trim for a target, within the trim's range; the analyzer", "[analysis][loudness]")
{
    CHECK(analysis::autoGainDb(-6.0, -10.0) == -4.0);
    CHECK(analysis::autoGainDb(-14.5, -10.0) == 4.5);
    CHECK(analysis::autoGainDb(-40.0, -10.0) == 12.0);
    CHECK(analysis::autoGainDb(5.0, -10.0) == -12.0);

    const auto x = sine(1000.0, -12.0, 10.0, 44100.0);
    analysis::DecodedAudio full{x.data(), x.data(), static_cast<int>(x.size()), 44100.0, {}};
    analysis::AnalysisInput input{full, {}};
    analysis::AnalysisResult result;
    analysis::AnalysisService::loudnessAnalyzer(input, result);
    REQUIRE(result.loudnessLufs.has_value());
    CHECK(std::abs(*result.loudnessLufs - -12.0) < 0.1);
    CHECK_FALSE(result.autoGainDb.has_value()); // the app works it out for the user's target

    const std::vector<float> silence(44100 * 5, 0.0f);
    analysis::DecodedAudio quiet{silence.data(), silence.data(), static_cast<int>(silence.size()), 44100.0, {}};
    analysis::AnalysisResult none;
    analysis::AnalysisService::loudnessAnalyzer(analysis::AnalysisInput{quiet, {}}, none);
    CHECK_FALSE(none.loudnessLufs.has_value());
}
