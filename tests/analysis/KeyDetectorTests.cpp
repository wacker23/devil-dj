// P2-15: key detection. Done when: all 24 generated chord loops detected correctly; eval-set accuracy reported
// (the owner's eval set: the hidden "[.eval]" test below; target >= 75 % exact, >= 90 % with relative / fifth).

#include "TestData.h"
#include "analysis/AnalysisService.h"
#include "analysis/KeyDetector.h"
#include "engine/TrackDecoder.h"
#include "library/TagReader.h"
#include "util/MusicalKey.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <random>

using namespace devildj;

namespace
{
constexpr double kRate = analysis::AnalysisInput::kMonoRate;

std::vector<float> loadMono(const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    REQUIRE(reader != nullptr);
    juce::AudioBuffer<float> buffer(2, static_cast<int>(reader->lengthInSamples));
    reader->read(&buffer, 0, buffer.getNumSamples(), 0, true, true);
    analysis::DecodedAudio a{buffer.getReadPointer(0), buffer.getReadPointer(1), buffer.getNumSamples(),
                             reader->sampleRate, {}};
    return analysis::AnalysisService::monoSignal(a);
}

analysis::KeyEstimate detect(const std::vector<float>& mono)
{
    return analysis::detectKey(mono.data(), static_cast<int>(mono.size()), kRate);
}

// A chord progression made here (cases the generated files don't cover: detuned, other voicings). Each note a
// harmonic tone (fundamental + 3 partials), `cents` off A = 440.
std::vector<float> progression(const std::vector<std::vector<int>>& chords, double chordSeconds, double cents,
                               int repeats = 2)
{
    const int perChord = static_cast<int>(chordSeconds * kRate);
    std::vector<float> out(static_cast<std::size_t>(perChord) * chords.size() * static_cast<std::size_t>(repeats));
    std::size_t at = 0;
    for (int r = 0; r < repeats; ++r)
        for (const auto& chord : chords)
        {
            for (const int note : chord)
            {
                const double hz = 440.0 * std::pow(2.0, (note - 69 + cents / 100.0) / 12.0);
                for (int i = 0; i < perChord; ++i)
                {
                    const double env = std::min(1.0, i / 220.0) * std::min(1.0, (perChord - i) / 220.0);
                    double v = 0.0;
                    for (const auto [h, amp] : {std::pair{1, 1.0}, {2, 0.5}, {3, 0.25}, {4, 0.12}})
                        v += amp * std::sin(2.0 * 3.141592653589793 * hz * h * i / kRate);
                    out[at + static_cast<std::size_t>(i)] += static_cast<float>(0.08 * env * v);
                }
            }
            at += static_cast<std::size_t>(perChord);
        }
    return out;
}
} // namespace

TEST_CASE("Key detection: all 24 generated chord loops", "[analysis][key]")
{
    const auto manifest = test::generatedManifest();
    if (!manifest.isObject())
        SKIP("Generated test signals missing (python tools/gen_test_signals.py)");
    int checked = 0;
    double lowestConfidence = 1.0, lowestLead = 1.0;
    for (const auto& entry : *manifest["files"].getArray())
    {
        if (entry["kind"].toString() != "key")
            continue;
        const auto props = entry["props"];
        const int expected = props["key_index"];
        const auto k = detect(loadMono(test::generatedFile(entry["file"].toString())));
        INFO(entry["file"].toString() << ": found " << util::musicalKeyName(k.key) << " (" << k.key << "), correlation "
                                      << k.correlation << ", confidence " << k.confidence << ", tuning "
                                      << k.tuningCents << " cents");
        REQUIRE(k.valid);
        CHECK(k.key == expected);
        CHECK(util::camelotCode(expected) == props["camelot"].toString().toStdString());
        CHECK(std::abs(k.tuningCents) < 3.0); // generated in tune
        CHECK(k.confidence > 0.2);
        double second = -1.0;
        for (int i = 0; i < 24; ++i)
            if (i != k.key)
                second = std::max(second, k.scores[static_cast<std::size_t>(i)]);
        lowestConfidence = std::min(lowestConfidence, k.confidence);
        lowestLead = std::min(lowestLead, k.correlation - second);
        ++checked;
    }
    CHECK(checked == 24);
    WARN("24 chord loops: lowest confidence " << lowestConfidence << ", smallest lead over the runner-up "
                                              << lowestLead);
}

TEST_CASE("Key detection: detuned recordings, other voicings, nothing tonal", "[analysis][key]")
{
    // A major (I-vi-IV-V) and F# minor (i-VI-iv-V), 35 cents sharp / 40 cents flat: the tuning is found and
    // taken out, the key stays.
    const std::vector<std::vector<int>> aMajor = {{45, 57, 61, 64}, {42, 54, 57, 61}, {38, 50, 54, 57}, {40, 52, 56, 59}};
    const std::vector<std::vector<int>> fsMinor = {{42, 54, 57, 61}, {38, 50, 54, 57}, {35, 47, 50, 54}, {37, 49, 53, 56}};
    for (const double cents : {0.0, 35.0, -40.0})
    {
        INFO(cents << " cents");
        const auto a = detect(progression(aMajor, 1.0, cents));
        CHECK(a.key == 9);
        CHECK(std::abs(a.tuningCents - cents) < 5.0);
        const auto fs = detect(progression(fsMinor, 1.0, cents));
        CHECK(fs.key == 12 + 6);
        CHECK(std::abs(fs.tuningCents - cents) < 5.0);
    }

    // Nothing tonal: silence and too short give no key; white noise gives a weak one at best.
    CHECK_FALSE(detect(std::vector<float>(static_cast<std::size_t>(kRate * 10), 0.0f)).valid);
    CHECK_FALSE(detect(std::vector<float>(4000, 0.1f)).valid);
    std::mt19937 rng(3);
    std::normal_distribution<float> noise(0.0f, 0.1f);
    std::vector<float> hiss(static_cast<std::size_t>(kRate * 10));
    for (auto& v : hiss)
        v = noise(rng);
    const auto n = detect(hiss);
    CHECK(n.confidence < detect(progression(aMajor, 1.0, 0.0)).confidence);
}

TEST_CASE("Key analyzer: writes key and confidence", "[analysis][key]")
{
    const std::vector<std::vector<int>> eMinor = {{40, 52, 55, 59}, {45, 57, 60, 64}, {47, 59, 63, 66}, {40, 52, 55, 59}};
    const auto mono = progression(eMinor, 1.0, 0.0);
    analysis::DecodedAudio full{mono.data(), mono.data(), static_cast<int>(mono.size()), kRate, {}};
    analysis::AnalysisInput input{full, mono};
    analysis::AnalysisResult result;
    analysis::AnalysisService::keyAnalyzer(input, result);
    REQUIRE(result.musicalKey.has_value());
    CHECK(*result.musicalKey == 12 + 4);
    REQUIRE(result.keyConfidence.has_value());
    CHECK(*result.keyConfidence > 0.0);
    CHECK(*result.keyConfidence <= 1.0);

    // Silence: nothing set (a key from the tags stays).
    const std::vector<float> silence(static_cast<std::size_t>(kRate * 5), 0.0f);
    analysis::DecodedAudio quiet{silence.data(), silence.data(), static_cast<int>(silence.size()), kRate, {}};
    analysis::AnalysisResult none;
    analysis::AnalysisService::keyAnalyzer(analysis::AnalysisInput{quiet, silence}, none);
    CHECK_FALSE(none.musicalKey.has_value());
}

TEST_CASE("Key names: musical and Camelot, related keys", "[util][key]")
{
    CHECK(util::musicalKeyName(0) == "C");
    CHECK(util::musicalKeyName(18) == "F#m");
    CHECK(util::musicalKeyName(10) == "Bb");
    CHECK(util::musicalKeyName(24).empty());
    CHECK(util::camelotCode(0) == "8B");
    CHECK(util::camelotCode(21) == "8A");  // A minor
    CHECK(util::camelotCode(7) == "9B");   // G major
    CHECK(util::camelotCode(16) == "9A");  // E minor
    CHECK(util::camelotCode(5) == "7B");   // F major
    CHECK(util::camelotCode(11) == "1B");  // B major
    CHECK(util::camelotCode(20) == "1A");  // G# minor
    CHECK(util::camelotCode(-1).empty());
    CHECK(util::keyName(21, util::KeyNotation::Camelot) == "8A");
    CHECK(util::keyName(21, util::KeyNotation::Musical) == "Am");
    for (int k = 0; k < 24; ++k)
    {
        INFO(k);
        // Every code parses back to the same key, both notations.
        CHECK(library::parseKeyTag(util::camelotCode(k)) == k);
        CHECK(library::parseKeyTag(util::musicalKeyName(k)) == k);
        CHECK(util::relativeKey(util::relativeKey(k)) == k);
        // The relative key shares the Camelot number.
        CHECK(util::camelotCode(util::relativeKey(k)).substr(0, util::camelotCode(k).size() - 1) ==
              util::camelotCode(k).substr(0, util::camelotCode(k).size() - 1));
    }
    CHECK(util::relativeKey(0) == 21);
    CHECK(util::isFifthApart(0, 7));
    CHECK(util::isFifthApart(0, 5));
    CHECK_FALSE(util::isFifthApart(0, 19));
    CHECK_FALSE(util::isFifthApart(0, 2));
}

// Hidden: the owner's evaluation set (P2-15 Done-when). DEVILDJ_EVAL_DIR points at a folder of real tracks; the
// known keys come from a key.csv in it ("file name,key" per line, key as "Am", "F#", "8A", ...) or, for files not
// listed, from their key tag. Writes eval-key-report.txt into the folder and prints the summary. Run by hand:
//   $env:DEVILDJ_EVAL_DIR = "D:\Eval"; DevilDJTests.exe "Key detection: the owner*"
TEST_CASE("Key detection: the owner's evaluation set", "[analysis][key][.eval]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable("DEVILDJ_EVAL_DIR", {});
    if (dir.isEmpty() || !juce::File(dir).isDirectory())
        SKIP("Set DEVILDJ_EVAL_DIR to the folder of evaluation tracks");
    const juce::File folder(dir);
    std::map<juce::String, int> known;
    if (const auto csv = folder.getChildFile("key.csv"); csv.existsAsFile())
    {
        juce::StringArray lines;
        csv.readLines(lines);
        for (const auto& line : lines)
            if (const auto name = line.upToLastOccurrenceOf(",", false, false).trim().unquoted(); name.isNotEmpty())
                if (const auto key = library::parseKeyTag(line.fromLastOccurrenceOf(",", false, false).trim().toStdString()))
                    known[name.toLowerCase()] = *key;
    }
    juce::String report = "Devil-DJ key detection evaluation (" + juce::Time::getCurrentTime().toString(true, true) +
                          ")\nfile, known key, found key, result, confidence, tuning cents\n";
    int total = 0, exact = 0, related = 0;
    for (const auto& file : folder.findChildFiles(juce::File::findFiles, true))
    {
        if (!engine::isSupportedTrackFile(file))
            continue;
        const auto name = file.getFileName().toLowerCase();
        int truth = known.count(name) > 0 ? known[name] : -1;
        if (truth < 0) // not in key.csv: the file's key tag
            truth = library::readTrackTags(std::filesystem::path(file.getFullPathName().toWideCharPointer()), false)
                        .musicalKey.value_or(-1);
        if (truth < 0)
            continue;
        auto decoded = engine::decodeTrack(file);
        if (!decoded)
            continue;
        const auto& buffer = decoded.value()->audio;
        analysis::DecodedAudio a{buffer.getReadPointer(0), buffer.getReadPointer(buffer.getNumChannels() > 1 ? 1 : 0),
                                 buffer.getNumSamples(), decoded.value()->sampleRate, {}};
        const auto k = detect(analysis::AnalysisService::monoSignal(a));
        const bool isExact = k.valid && k.key == truth;
        const bool isRelated = k.valid && !isExact &&
                               (k.key == util::relativeKey(truth) || util::isFifthApart(k.key, truth));
        ++total;
        exact += isExact ? 1 : 0;
        related += isRelated ? 1 : 0;
        report << file.getFileName() << ", " << util::musicalKeyName(truth) << " (" << util::camelotCode(truth) << "), "
               << util::musicalKeyName(k.key) << " (" << util::camelotCode(k.key) << "), "
               << (isExact ? "exact" : isRelated ? "related" : "NO") << ", " << juce::String(k.confidence, 2) << ", "
               << juce::String(k.tuningCents, 1) << "\n";
    }
    const auto pct = [&](int n) { return juce::String(total > 0 ? 100.0 * n / total : 0.0, 1); };
    report << "\n" << exact << " of " << total << " exact (" << pct(exact) << " %), " << exact + related
           << " with relative / fifth (" << pct(exact + related) << " %)\n";
    folder.getChildFile("eval-key-report.txt").replaceWithText(report);
    WARN(report);
    CHECK(total >= 50);
    CHECK(exact >= 0.75 * total);
    CHECK(exact + related >= 0.9 * total);
}
