// P2-13: beat tracker v1. Done when: on generated click tracks BPM error < 0.02 and first beat < 5 ms (the
// owner's eval set: the hidden "[.eval]" test below, report in DEVLOG).

#include "TestData.h"
#include "analysis/AnalysisService.h"
#include "analysis/BeatTracker.h"
#include "engine/TrackDecoder.h"
#include "library/TagReader.h"
#include "util/Beatgrid.h"

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

// A drum kit played to the given beat times: kick on beats 1 and 3, snare on 2 and 4, hi-hat on every eighth.
// Each hit is moved by a random human timing error (standard deviation `jitter` seconds).
std::vector<float> drums(const std::vector<double>& beats, double seconds, double jitter, unsigned seed)
{
    constexpr double rate = 22050.0;
    std::vector<float> out(static_cast<std::size_t>(seconds * rate));
    std::mt19937 rng(seed);
    std::normal_distribution<double> timing(0.0, jitter > 0.0 ? jitter : 1e-9);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    const auto hit = [&](double at, int kind)
    {
        const int start = static_cast<int>((at + (jitter > 0.0 ? timing(rng) : 0.0)) * rate);
        const int length = static_cast<int>((kind == 0 ? 0.15 : kind == 1 ? 0.12 : 0.03) * rate);
        float previous = 0.0f;
        for (int i = 0; i < length; ++i)
        {
            const auto at_ = static_cast<std::size_t>(start + i);
            if (start + i < 0 || at_ >= out.size())
                continue;
            const double t = i / rate;
            double v = 0.0;
            if (kind == 0) // kick: a falling 100 -> 50 Hz thump
                v = 0.9 * std::exp(-t / 0.05) * std::sin(2.0 * 3.141592653589793 * (50.0 * t + 50.0 * 0.03 * (1.0 - std::exp(-t / 0.03))));
            else if (kind == 1) // snare: noise and a 200 Hz body
                v = std::exp(-t / 0.04) * (0.5 * noise(rng) + 0.3 * std::sin(2.0 * 3.141592653589793 * 200.0 * t));
            else // hi-hat: high-passed noise
            {
                const float n = noise(rng);
                v = 0.25 * std::exp(-t / 0.008) * (n - previous);
                previous = n;
            }
            out[at_] += static_cast<float>(v);
        }
    };
    for (std::size_t b = 0; b < beats.size(); ++b)
    {
        hit(beats[b], b % 2 == 0 ? 0 : 1);
        hit(beats[b], 2);
        if (b + 1 < beats.size())
            hit(0.5 * (beats[b] + beats[b + 1]), 2);
    }
    return out;
}

// The share of the true beats that the analyzed grid (through the library's JSON, as a deck gets it) puts a beat
// within `tolerance` seconds of, leaving out the first and last 2 s.
double onGrid(const analysis::BeatGrid& g, const std::vector<double>& beats, double tolerance)
{
    const auto grid = util::Beatgrid::fromJson(analysis::beatgridJson(g, 22050.0), 22050.0);
    REQUIRE(grid.has_value());
    int hits = 0, counted = 0;
    for (const double t : beats)
    {
        if (t < beats.front() + 2.0 || t > beats.back() - 2.0)
            continue;
        const double nearest = grid->sampleAt(std::round(grid->beatAt(t * 22050.0))) / 22050.0;
        hits += std::abs(nearest - t) <= tolerance ? 1 : 0;
        ++counted;
    }
    return counted > 0 ? static_cast<double>(hits) / counted : 0.0;
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

TEST_CASE("Beat tracker: a live band's wandering tempo gets a grid that follows it", "[analysis][beats]")
{
    // 2 minutes of drums around 120 BPM, the tempo wandering about +-3 % over tens of seconds (as a band without a
    // click does), every hit off by a human 6 ms.
    std::vector<double> beats;
    for (double t = 0.5; t < 118.0;)
    {
        beats.push_back(t);
        t += 60.0 / (120.0 + 3.0 * std::sin(2.0 * 3.141592653589793 * t / 90.0) +
                     1.0 * std::sin(2.0 * 3.141592653589793 * t / 37.0));
    }
    const auto live = track(drums(beats, 120.0, 0.006, 11));
    INFO(live.bpm << " BPM, constant grid hits " << live.constantHitRate << ", following " << live.followHitRate
                  << ", steady share " << live.steadyShare << ", " << live.anchors.size() << " anchors");
    REQUIRE(live.valid);
    CHECK(live.variableTempo);
    CHECK_FALSE(live.anchors.empty());
    CHECK(live.anchors.size() <= 16);
    const double average = 60.0 * static_cast<double>(beats.size() - 1) / (beats.back() - beats.front());
    CHECK(std::abs(live.bpm - average) < 0.5);
    // The grid stays on the band: nearly every beat within 25 ms, where a constant grid at the same average
    // tempo loses most of them.
    const double following = onGrid(live, beats, 0.025);
    auto constant = live;
    constant.anchors.clear();
    const double straight = onGrid(constant, beats, 0.025);
    WARN("live band: " << 100.0 * following << " % of the beats within 25 ms of the following grid ("
                       << live.anchors.size() << " anchors), " << 100.0 * straight << " % of a constant grid");
    CHECK(following >= 0.9);
    CHECK(straight < 0.5);
}

TEST_CASE("Beat tracker: steady programmed drums stay one constant grid", "[analysis][beats]")
{
    std::vector<double> beats;
    for (double t = 0.4; t < 89.0; t += 60.0 / 124.0)
        beats.push_back(t);
    const auto steady = track(drums(beats, 90.0, 0.0, 5));
    INFO(steady.bpm << " BPM, steady share " << steady.steadyShare << ", constant grid hits "
                    << steady.constantHitRate);
    REQUIRE(steady.valid);
    CHECK_FALSE(steady.variableTempo);
    CHECK(steady.anchors.empty());
    CHECK(std::abs(steady.bpm - 124.0) < 0.02);
    CHECK(onGrid(steady, beats, 0.005) >= 0.99);
    // A variable grid's JSON: one anchor per tempo change, each with its bar position.
    analysis::BeatGrid g;
    g.valid = true;
    g.bpm = 120.0;
    g.firstBeatSeconds = 0.5;
    g.firstBeatInBar = 2;
    g.anchors = {{0, 0.5, 120.0}, {10, 5.5, 122.0}};
    CHECK(analysis::beatgridJson(g, 44100.0) == "[{\"beat\":0,\"sample\":22050,\"bpm\":120.0000,\"bar_beat\":2},"
                                                "{\"beat\":10,\"sample\":242550,\"bpm\":122.0000,\"bar_beat\":4}]");
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
                          ")\nfile, known BPM, found BPM, error, within 0.5, first beat s, variable, tempogram BPM, beats used, drift %, constant fraction, constant grid hits %, following grid hits %, steady share, anchors\n";
    int total = 0, within = 0, octave = 0, untagged = 0, undecodable = 0;
    for (const auto& file : folder.findChildFiles(juce::File::findFiles, true))
    {
        if (!engine::isSupportedTrackFile(file))
            continue;
        double truth = known.count(file.getFileName().toLowerCase()) > 0 ? known[file.getFileName().toLowerCase()] : 0.0;
        if (truth <= 0.0) // not in bpm.csv: the file's BPM tag
            truth = library::readBasicTags(std::filesystem::path(file.getFullPathName().toWideCharPointer())).bpm;
        if (truth <= 0.0)
        {
            ++untagged;
            continue;
        }
        auto decoded = engine::decodeTrack(file); // the app's decoder (MP3 / AAC through Media Foundation)
        if (!decoded)
        {
            ++undecodable;
            continue;
        }
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
               << ", " << (grid.variableTempo ? "variable" : "") << ", " << juce::String(grid.tempogramBpm, 2) << ", "
               << grid.beatsUsed << ", " << juce::String(grid.maxDriftPercent, 1) << ", " << juce::String(grid.constantFraction, 2) << ", " << juce::String(100.0 * grid.constantHitRate, 0) << ", " << juce::String(100.0 * grid.followHitRate, 0)
               << ", " << juce::String(grid.steadyShare, 2) << ", " << static_cast<int>(grid.anchors.size()) << "\n";
    }
    report << "\n" << within << " of " << total << " within +-0.5 BPM ("
           << juce::String(total > 0 ? 100.0 * within / total : 0.0, 1) << " %), " << octave << " octave errors; skipped: " << untagged << " without a BPM, " << undecodable
           << " not decodable\n";
    folder.getChildFile("eval-bpm-report.txt").replaceWithText(report);
    WARN(report);
    CHECK(total >= 50);
    CHECK(within >= 0.9 * total);
}
