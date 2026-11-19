// P2-14: the metronome. Done when: the metronome stays on the grid for 10 minutes on a 128 BPM click track.

#include "analysis/AnalysisService.h"
#include "analysis/BeatTracker.h"
#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace devildj;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

// A click track: 10 ms decaying 1 kHz bursts on every beat, the first at `first` seconds.
std::unique_ptr<engine::TrackBuffer> clickTrack(double bpm, double seconds, double first)
{
    auto t = std::make_unique<engine::TrackBuffer>();
    t->sampleRate = kRate;
    const int n = static_cast<int>(seconds * kRate);
    t->audio.setSize(2, n);
    t->audio.clear();
    for (double at = first; at < seconds - 0.1; at += 60.0 / bpm)
    {
        const int start = static_cast<int>(std::lround(at * kRate));
        for (int i = 0; i < 480 && start + i < n; ++i)
        {
            const auto v = static_cast<float>(0.5 * std::exp(-i / 96.0) * std::sin(6.283185307 * 1000.0 * i / kRate));
            t->audio.setSample(0, start + i, v);
            t->audio.setSample(1, start + i, v);
        }
    }
    return t;
}

// Where clicks start in a signal: the first sample over a threshold after at least `gap` quiet samples.
std::vector<int> onsets(const std::vector<float>& x, float threshold, int gap)
{
    std::vector<int> out;
    int quiet = gap;
    for (int i = 0; i < static_cast<int>(x.size()); ++i)
    {
        if (std::abs(x[static_cast<std::size_t>(i)]) > threshold)
        {
            if (quiet >= gap)
                out.push_back(i);
            quiet = 0;
        }
        else
        {
            ++quiet;
        }
    }
    return out;
}

struct Run
{
    std::vector<int> track, metronome; // output sample of each click
};

Run play(const engine::TrackBuffer& track, const util::Beatgrid& grid, double tempoPercent, double seconds)
{
    engine::AudioEngine engine;
    engine.prepare(kRate, kBlock);
    engine.send(engine::SetOutputLayout{engine::OutputLayout::FourChannel}); // master 1/2, headphones 3/4
    engine.send(engine::SetParam{engine::ParamId::CueMix, 0.0f});           // headphones: the cue bus only
    engine.send(engine::LoadTrack{0, &track, 1});
    engine.send(engine::SetBeatgrid{0, grid});
    engine.send(engine::SetMetronome{0, true, -6.0f});
    engine.send(engine::SetTempo{0, tempoPercent});
    engine.send(engine::Play{0});

    std::vector<float> out[4];
    for (auto& o : out)
        o.assign(kBlock, 0.0f);
    float* ptrs[4] = {out[0].data(), out[1].data(), out[2].data(), out[3].data()};
    std::vector<float> master, cue;
    const int blocks = static_cast<int>(seconds * kRate / kBlock);
    master.reserve(static_cast<std::size_t>(blocks) * kBlock);
    cue.reserve(static_cast<std::size_t>(blocks) * kBlock);
    for (int b = 0; b < blocks; ++b)
    {
        engine.process(ptrs, 4, kBlock);
        master.insert(master.end(), out[0].begin(), out[0].end());
        cue.insert(cue.end(), out[2].begin(), out[2].end());
    }
    Run r;
    r.track = onsets(master, 0.05f, 2400);  // 50 ms of quiet between clicks
    r.metronome = onsets(cue, 0.05f, 2400);
    return r;
}

// The grid the analysis finds for the track (as the app would have it).
util::Beatgrid analyzedGrid(const engine::TrackBuffer& track)
{
    analysis::DecodedAudio a{track.audio.getReadPointer(0), track.audio.getReadPointer(1), track.audio.getNumSamples(),
                             track.sampleRate, {}};
    const auto mono = analysis::AnalysisService::monoSignal(a);
    const auto g = analysis::trackBeats(mono.data(), static_cast<int>(mono.size()), analysis::AnalysisInput::kMonoRate);
    REQUIRE(g.valid);
    return util::Beatgrid::constant(g.firstBeatSeconds * track.sampleRate, g.bpm, track.sampleRate, g.firstBeatInBar);
}

void checkAligned(const Run& r, int expectedClicks, double toleranceMs)
{
    // Each of the track's clicks against the nearest metronome click. (The grid goes on before the first and after
    // the last beat, so the metronome may click once more at either end, over the track's silence.)
    REQUIRE(r.track.size() >= static_cast<std::size_t>(expectedClicks));
    REQUIRE(r.metronome.size() >= r.track.size());
    REQUIRE(r.metronome.size() <= r.track.size() + 2);
    double worst = 0.0, first = 0.0, last = 0.0;
    std::size_t m = 0;
    for (std::size_t i = 0; i < r.track.size(); ++i)
    {
        while (m + 1 < r.metronome.size() &&
               std::abs(r.metronome[m + 1] - r.track[i]) <= std::abs(r.metronome[m] - r.track[i]))
            ++m;
        const double ms = 1000.0 * (r.metronome[m] - r.track[i]) / kRate;
        worst = std::max(worst, std::abs(ms));
        if (i == 0)
            first = ms;
        last = ms;
    }
    WARN(r.track.size() << " beats: metronome vs track worst " << worst << " ms, first " << first << " ms, last " << last
                        << " ms");
    CHECK(worst < toleranceMs);
    CHECK(std::abs(last - first) < 0.5); // no drift over the whole run
}
} // namespace

TEST_CASE("Metronome: 10 minutes on a 128 BPM click track, on the analyzed grid", "[engine][metronome]")
{
#if JUCE_DEBUG
    constexpr double kSeconds = 120.0; // Debug plays ~10x slower (ctest's 120 s limit); Release does the full 10 minutes
#else
    constexpr double kSeconds = 600.0;
#endif
    const auto track = clickTrack(128.0, kSeconds, 0.5);
    const auto grid = analyzedGrid(*track);
    CHECK(std::abs(grid.bpmAt(0.0) - 128.0) < 0.01);
    const auto run = play(*track, grid, 0.0, kSeconds);
    checkAligned(run, static_cast<int>(128.0 * kSeconds / 60.0) - 10, 1.0); // all the beats but the last second
}

TEST_CASE("Metronome: stays on the beats at another tempo, quiet when off or paused", "[engine][metronome]")
{
    const auto track = clickTrack(128.0, 60.0, 0.5);
    const auto grid = util::Beatgrid::constant(0.5 * kRate, 128.0, kRate);
    // +6 %: the track plays faster, the clicks follow its beats.
    checkAligned(play(*track, grid, 6.0, 50.0), 100, 1.0);

    // Downbeats click higher (2 kHz) and louder than the other beats.
    {
        engine::AudioEngine engine;
        engine.prepare(kRate, kBlock);
        engine.send(engine::SetOutputLayout{engine::OutputLayout::FourChannel});
        engine.send(engine::SetParam{engine::ParamId::CueMix, 0.0f});
        engine.send(engine::LoadTrack{0, track.get(), 1});
        engine.send(engine::SetBeatgrid{0, grid});
        engine.send(engine::SetMetronome{0, true, -6.0f});
        engine.send(engine::Play{0});
        std::vector<float> o[4];
        for (auto& v : o)
            v.assign(kBlock, 0.0f);
        float* p[4] = {o[0].data(), o[1].data(), o[2].data(), o[3].data()};
        std::vector<float> cue;
        for (int b = 0; b < static_cast<int>(4.0 * kRate / kBlock); ++b)
        {
            engine.process(p, 4, kBlock);
            cue.insert(cue.end(), o[2].begin(), o[2].end());
        }
        const auto clicks = onsets(cue, 0.05f, 2400);
        REQUIRE(clicks.size() >= 8);
        std::vector<float> peaks;
        for (const int c : clicks)
        {
            float peak = 0.0f;
            for (int i = c; i < c + 480 && i < static_cast<int>(cue.size()); ++i)
                peak = std::max(peak, std::abs(cue[static_cast<std::size_t>(i)]));
            peaks.push_back(peak);
        }
        // Which beat each click is (the first beat is at 0.5 s, a downbeat): downbeats louder than the rest.
        float downbeat = 1e9f, other = 0.0f;
        for (std::size_t i = 0; i < clicks.size(); ++i)
        {
            const auto k = std::lround((clicks[i] / kRate - 0.5) / (60.0 / 128.0));
            if (((k % 4) + 4) % 4 == 0)
                downbeat = std::min(downbeat, peaks[i]);
            else
                other = std::max(other, peaks[i]);
        }
        CHECK(downbeat > other * 1.2f);
        CHECK(engine.readState().decks[0].metronome);
        CHECK(engine.readState().decks[0].hasGrid);

        // Off: silence on the cue bus.
        engine.send(engine::SetMetronome{0, false, -6.0f});
        cue.clear();
        for (int b = 0; b < static_cast<int>(2.0 * kRate / kBlock); ++b)
        {
            engine.process(p, 4, kBlock);
            cue.insert(cue.end(), o[2].begin(), o[2].end());
        }
        float loudest = 0.0f;
        for (std::size_t i = 4800; i < cue.size(); ++i)
            loudest = std::max(loudest, std::abs(cue[i]));
        CHECK(loudest < 1e-4f);

        // A new track clears the grid until the app sends the new one.
        engine.send(engine::LoadTrack{0, track.get(), 2});
        for (int b = 0; b < 4; ++b)
            engine.process(p, 4, kBlock);
        CHECK_FALSE(engine.readState().decks[0].hasGrid);
    }
}
