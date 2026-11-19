// P2-13: beat tracker v1. Done when: on generated click tracks BPM error < 0.02 and first beat < 5 ms (the
// owner's eval set: the hidden "[.eval]" test below, report in DEVLOG).

#include "TestData.h"
#include "analysis/AnalysisService.h"
#include "analysis/BeatTracker.h"
#include "engine/TrackDecoder.h"
#include "library/TagReader.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <map>
#include <random>

using namespace devildj;

namespace
{
struct Signal
{
    std::vector<float> mono; // 22.05 kHz, as the analysis service makes it
    double fileRate = 0.0;
};

Signal load(const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    REQUIRE(reader != nullptr);
    juce::AudioBuffer<float> buffer(2, static_cast<int>(reader->lengthInSamples));
    reader->read(&buffer, 0, buffer.getNumSamples(), 0, true, true);
    analysis::DecodedAudio a;
    a.left = buffer.getReadPointer(0);
    a.right = buffer.getReadPointer(1);
    a.numSamples = buffer.getNumSamples();
    a.sampleRate = reader->sampleRate;
    return {analysis::AnalysisService::monoSignal(a), reader->sampleRate};
}

analysis::BeatGrid track(const std::vector<float>& mono)
{
    return analysis::trackBeats(mono.data(), static_cast<int>(mono.size()), analysis::AnalysisInput::kMonoRate);
}

// A click track made here (for cases the generated files don't cover): beat times given, 10 ms bursts at 1 kHz,
// accented (2 kHz, louder) every 4th beat.
std::vector<float> clicks(const std::vector<double>& beats, double seconds, double rate = 22050.0)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * rate));
    for (std::size_t b = 0; b < beats.size(); ++b)
    {
        const bool accent = b % 4 == 0;
        const int start = static_cast<int>(beats[b] * rate);
        for (int i = 0; i < static_cast<int>(0.01 * rate) && start + i < static_cast<int>(out.size()); ++i)
            out[static_cast<std::size_t>(start + i)] +=
                static_cast<float>((accent ? 0.8 : 0.5) * std::exp(-i / (0.002 * rate)) *
                                   std::sin(6.283185307 * (accent ? 2000.0 : 1000.0) * i / rate));
    }
    return out;
}
} // namespace

TEST_CASE("Beat tracker: generated click tracks - BPM within 0.02, first beat within 5 ms, downbeat", "[analysis][beats]")
{
    const auto manifest = test::generatedManifest();
    if (!manifest.isObject())
        SKIP("Generated test signals missing (python tools/gen_test_signals.py)");
    int checked = 0;
    for (const auto& entry : *manifest["files"].getArray())
    {
        if (entry["kind"].toString() != "click")
            continue;
        const auto props = entry["props"];
        const double bpm = props["bpm"];
        const double firstBeat = props["first_beat_sec"];
        const auto signal = load(test::generatedFile(entry["file"].toString()));
        const auto grid = track(signal.mono);
        INFO(entry["file"].toString() << ": " << grid.bpm << " BPM (tempogram " << grid.tempogramBpm << "), first beat "
                                      << grid.firstBeatSeconds << " s, beat in bar " << grid.firstBeatInBar);
        REQUIRE(grid.valid);
        CHECK(std::abs(grid.bpm - bpm) < 0.02);
        CHECK(std::abs(grid.firstBeatSeconds - firstBeat) < 0.005);
        CHECK_FALSE(grid.variableTempo);
        if (props.hasProperty("downbeat_every"))
            CHECK(grid.firstBeatInBar == 1); // the first click is an accented downbeat
        WARN(entry["file"].toString() << ": " << grid.bpm << " BPM (error " << grid.bpm - bpm << "), first beat error "
                                      << 1000.0 * (grid.firstBeatSeconds - firstBeat) << " ms, drift "
                                      << grid.maxDriftPercent << " %");
        ++checked;
    }
    CHECK(checked == 6); // 5 click tracks + the padded one
}

TEST_CASE("Beat tracker: downbeat phase, tempo drift, octaves, no beat", "[analysis][beats]")
{
    // The accent on the 3rd click: the first beat is beat 3 of its bar.
    {
        std::vector<double> beats;
        for (int i = 0; i < 64; ++i)
            beats.push_back(0.3 + i * 0.5);
        auto x = clicks({}, 33.0);
        for (std::size_t b = 0; b < beats.size(); ++b)
        {
            const bool accent = b % 4 == 2;
            const int start = static_cast<int>(beats[b] * 22050.0);
            for (int i = 0; i < 220; ++i)
                x[static_cast<std::size_t>(start + i)] += static_cast<float>((accent ? 0.9 : 0.4) * std::exp(-i / 44.0) *
                                                                            std::sin(6.283185307 * 1000.0 * i / 22050.0));
        }
        const auto grid = track(x);
        REQUIRE(grid.valid);
        CHECK(std::abs(grid.bpm - 120.0) < 0.02);
        CHECK(grid.firstBeatInBar == 3);
    }
    // Tempo drifting from 120 to 126 BPM: flagged as variable.
    {
        std::vector<double> beats;
        double t = 0.5;
        for (int i = 0; i < 120; ++i)
        {
            beats.push_back(t);
            t += 60.0 / (120.0 + 6.0 * i / 120.0);
        }
        const auto grid = track(clicks(beats, t + 1.0));
        REQUIRE(grid.valid);
        CHECK(grid.variableTempo);
        CHECK(grid.maxDriftPercent > 1.0);
    }
    // Steady 128 with a little timing jitter (+-2 ms): not variable, BPM still close.
    {
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> jitter(-0.002, 0.002);
        std::vector<double> beats;
        for (int i = 0; i < 100; ++i)
            beats.push_back(0.4 + i * 60.0 / 128.0 + jitter(rng));
        const auto grid = track(clicks(beats, 48.0));
        REQUIRE(grid.valid);
        CHECK(std::abs(grid.bpm - 128.0) < 0.05);
        CHECK_FALSE(grid.variableTempo);
    }
    // Octaves: 87 BPM and 174 BPM click tracks keep their own tempo; 60 BPM doubles into the preferred range.
    for (const auto [bpm, expected] : {std::pair{87.0, 87.0}, std::pair{174.0, 174.0}, std::pair{60.0, 120.0}})
    {
        std::vector<double> beats;
        for (double t = 0.5; t < 39.0; t += 60.0 / bpm)
            beats.push_back(t);
        const auto grid = track(clicks(beats, 40.0));
        INFO(bpm << " BPM -> " << grid.bpm);
        REQUIRE(grid.valid);
        CHECK(std::abs(grid.bpm - expected) < 0.05);
    }
    // Nothing steady: silence and noise.
    {
        const std::vector<float> silence(22050 * 20, 0.0f);
        CHECK_FALSE(track(silence).valid);
        std::mt19937 rng(1);
        std::normal_distribution<float> noise(0.0f, 0.1f);
        std::vector<float> hiss(22050 * 20);
        for (auto& v : hiss)
            v = noise(rng);
        CHECK_FALSE(track(hiss).valid);
        const std::vector<float> tooShort = clicks({0.5, 1.0, 1.5, 2.0}, 3.0);
        CHECK_FALSE(track(tooShort).valid);
    }
}

TEST_CASE("Beat tracker: the beatgrid JSON", "[analysis][beats]")
{
    analysis::BeatGrid g;
    g.valid = true;
    g.bpm = 128.0;
    g.firstBeatSeconds = 0.5;
    g.firstBeatInBar = 1;
    CHECK(analysis::beatgridJson(g, 44100.0) == "[{\"beat\":0,\"sample\":22050,\"bpm\":128.0000,\"bar_beat\":1}]");
    g.valid = false;
    CHECK(analysis::beatgridJson(g, 44100.0) == "[]");
}

// Hidden: the owner's evaluation set (P2-13 Done-when). DEVILDJ_EVAL_DIR points at a folder of real tracks; the
// known BPMs come from a bpm.csv in it ("file name,bpm" per line) or, for files not listed, from their BPM tag.
// Writes eval-bpm-report.txt into the folder and prints the summary. Run by hand:
//   $env:DEVILDJ_EVAL_DIR = "D:\Eval"; DevilDJTests.exe "[.eval]"
TEST_CASE("Beat tracker: the owner's evaluation set", "[analysis][beats][.eval]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable("DEVILDJ_EVAL_DIR", {});
    if (dir.isEmpty() || !juce::File(dir).isDirectory())
        SKIP("Set DEVILDJ_EVAL_DIR to the folder of evaluation tracks");
    const juce::File folder(dir);
    std::map<juce::String, double> known;
    if (const auto csv = folder.getChildFile("bpm.csv"); csv.existsAsFile())
    {
        juce::StringArray lines;
        csv.readLines(lines);
        for (const auto& line : lines)
            if (const auto name = line.upToLastOccurrenceOf(",", false, false).trim().unquoted(); name.isNotEmpty())
                if (const double bpm = line.fromLastOccurrenceOf(",", false, false).trim().getDoubleValue(); bpm > 0.0)
                    known[name.toLowerCase()] = bpm;
    }
    juce::String report = "Devil-DJ beat tracker v1 evaluation (" + juce::Time::getCurrentTime().toString(true, true) +
                          ")\nfile, known BPM, found BPM, error, within 0.5, first beat s, variable\n";
    int total = 0, within = 0, octave = 0;
    for (const auto& file : folder.findChildFiles(juce::File::findFiles, true))
    {
        if (!engine::isSupportedTrackFile(file))
            continue;
        double truth = known.count(file.getFileName().toLowerCase()) > 0 ? known[file.getFileName().toLowerCase()] : 0.0;
        if (truth <= 0.0) // not in bpm.csv: the file's BPM tag
            truth = library::readBasicTags(std::filesystem::path(file.getFullPathName().toWideCharPointer())).bpm;
        if (truth <= 0.0)
            continue;
        auto decoded = engine::decodeTrack(file); // the app's decoder (MP3 / AAC through Media Foundation)
        if (!decoded)
            continue;
        const auto& buffer = decoded.value()->audio;
        analysis::DecodedAudio a{buffer.getReadPointer(0), buffer.getReadPointer(buffer.getNumChannels() > 1 ? 1 : 0),
                                 buffer.getNumSamples(), decoded.value()->sampleRate, {}};
        const auto grid = track(analysis::AnalysisService::monoSignal(a));
        const double err = grid.bpm - truth;
        const bool ok = grid.valid && std::abs(err) <= 0.5;
        const bool octaveErr = grid.valid && !ok &&
                               (std::abs(grid.bpm * 2.0 - truth) <= 1.0 || std::abs(grid.bpm / 2.0 - truth) <= 0.5);
        ++total;
        within += ok ? 1 : 0;
        octave += octaveErr ? 1 : 0;
        report << file.getFileName() << ", " << truth << ", " << juce::String(grid.bpm, 2) << ", " << juce::String(err, 2)
               << ", " << (ok ? "yes" : octaveErr ? "octave" : "NO") << ", " << juce::String(grid.firstBeatSeconds, 3)
               << ", " << (grid.variableTempo ? "variable" : "") << "\n";
    }
    report << "\n" << within << " of " << total << " within +-0.5 BPM ("
           << juce::String(total > 0 ? 100.0 * within / total : 0.0, 1) << " %), " << octave << " octave errors\n";
    folder.getChildFile("eval-bpm-report.txt").replaceWithText(report);
    WARN(report);
    CHECK(total >= 50);
    CHECK(within >= 0.9 * total);
}
