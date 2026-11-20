// P2-19: beat sync. Done when: two click tracks at 124 and 128 BPM, synced, stay within 1 ms for 10 minutes
// (automated engine test, offline render).

#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace devildj;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

// 10 ms decaying bursts on every beat (1 kHz), the first at `first` seconds.
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

// Where clicks start: the first sample over the threshold after 50 ms of quiet; sub-sample by the crossing.
std::vector<double> onsets(const std::vector<float>& x)
{
    std::vector<double> out;
    int quiet = 2400;
    for (int i = 1; i < static_cast<int>(x.size()); ++i)
    {
        const float v = std::abs(x[static_cast<std::size_t>(i)]);
        if (v > 0.05f)
        {
            if (quiet >= 2400)
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

// Deck A (master bus only: crossfader to A) and deck B (headphones only: cue on, cue mix to cue) in one engine,
// so each deck's clicks can be found on its own output.
struct Rig
{
    engine::AudioEngine engine;
    std::vector<float> out[4];
    float* ptrs[4];
    std::vector<float> a, b; // master (deck A), headphones (deck B)

    Rig()
    {
        engine.prepare(kRate, kBlock);
        for (int i = 0; i < 4; ++i)
        {
            out[i].assign(kBlock, 0.0f);
            ptrs[i] = out[i].data();
        }
        engine.send(engine::SetOutputLayout{engine::OutputLayout::FourChannel});
        engine.send(engine::SetParam{engine::ParamId::Crossfader, 0.0f}); // A side only on the master
        engine.send(engine::SetParam{engine::ParamId::CueMix, 0.0f});     // headphones: the cue bus only
        engine.send(engine::SetParam{engine::channelParam(1, engine::ChannelParam::Cue), 1.0f});
    }
    void load(engine::DeckId deck, const engine::TrackBuffer& t, double firstBeat, double bpm)
    {
        engine.send(engine::LoadTrack{deck, &t, static_cast<engine::BufferId>(deck + 1)});
        engine.send(engine::SetBeatgrid{deck, util::Beatgrid::constant(firstBeat * kRate, bpm, kRate)});
    }
    void run(double seconds, bool record = true)
    {
        const int blocks = static_cast<int>(seconds * kRate / kBlock);
        for (int i = 0; i < blocks; ++i)
        {
            engine.process(ptrs, 4, kBlock);
            if (record)
            {
                a.insert(a.end(), out[0].begin(), out[0].end());
                b.insert(b.end(), out[2].begin(), out[2].end());
            }
            else
            {
                a.resize(a.size() + kBlock, 0.0f);
                b.resize(b.size() + kBlock, 0.0f);
            }
        }
    }
    const engine::DeckState& deck(int i) { return engine.readState().decks[static_cast<std::size_t>(i)]; }
};

struct Alignment
{
    int pairs = 0;
    double worstMs = 0.0, firstMs = 0.0, lastMs = 0.0;
};

// Each of A's clicks after `fromSeconds` against B's nearest; `every` = B clicks once per that many A clicks.
Alignment align(const std::vector<double>& a, const std::vector<double>& b, double fromSeconds)
{
    Alignment r;
    std::size_t j = 0;
    for (const double t : a)
    {
        if (t < fromSeconds * kRate || b.empty())
            continue;
        while (j + 1 < b.size() && std::abs(b[j + 1] - t) <= std::abs(b[j] - t))
            ++j;
        const double ms = 1000.0 * (b[j] - t) / kRate;
        if (std::abs(ms) > 100.0) // B has no click near this one (half-time, or the end of a track)
            continue;
        if (r.pairs == 0)
            r.firstMs = ms;
        r.lastMs = ms;
        r.worstMs = std::max(r.worstMs, std::abs(ms));
        ++r.pairs;
    }
    return r;
}
} // namespace

TEST_CASE("Sync: 124 and 128 BPM click tracks stay within 1 ms (10 minutes, offline render)", "[engine][sync]")
{
#if JUCE_DEBUG
    constexpr double kSeconds = 120.0; // Debug renders ~10x slower (ctest's limit); Release: the full 10 minutes
#else
    constexpr double kSeconds = 600.0;
#endif
    // Deck B's track is longer: at 128 / 124 it plays faster.
    const auto trackA = clickTrack(128.0, kSeconds + 5.0, 0.5);
    const auto trackB = clickTrack(124.0, (kSeconds + 5.0) * 1.04, 0.3);
    Rig rig;
    rig.load(0, *trackA, 0.5, 128.0);
    rig.load(1, *trackB, 0.3, 124.0);
    rig.engine.send(engine::Play{0});
    rig.run(1.37); // A plays alone for a while: B starts somewhere in the middle of a beat
    rig.engine.send(engine::SetSync{1, true});
    rig.engine.send(engine::Play{1});
    rig.run(kSeconds);

    CHECK(rig.deck(0).master);
    CHECK(rig.deck(1).synced);
    CHECK_FALSE(rig.deck(1).master);
    CHECK(std::abs(rig.deck(1).tempoPercent - (128.0 / 124.0 - 1.0) * 100.0) < 1e-6);
    const auto r = align(onsets(rig.a), onsets(rig.b), 1.6); // from the first beat after B started
    WARN(r.pairs << " beats synced: worst " << r.worstMs << " ms, first " << r.firstMs << " ms, last " << r.lastMs
                 << " ms, phase error now " << rig.deck(1).phaseErrorMs << " ms");
    CHECK(r.pairs >= static_cast<int>(128.0 * (kSeconds - 1.0) / 60.0));
    CHECK(r.worstMs < 1.0);
}

TEST_CASE("Sync: half / double tempo, small errors close within a beat, and sync lock", "[engine][sync]")
{
    // A 64 BPM track syncs to a 128 BPM master at its own tempo, on every second beat.
    {
        const auto trackA = clickTrack(128.0, 40.0, 0.5);
        const auto trackB = clickTrack(64.0, 40.0, 0.8);
        Rig rig;
        rig.load(0, *trackA, 0.5, 128.0);
        rig.load(1, *trackB, 0.8, 64.0);
        rig.engine.send(engine::Play{0});
        rig.run(0.9);
        rig.engine.send(engine::SetSync{1, true});
        rig.engine.send(engine::Play{1});
        rig.run(20.0);
        CHECK(std::abs(rig.deck(1).tempoPercent) < 1e-9);
        const auto r = align(onsets(rig.a), onsets(rig.b), 2.0);
        CHECK(r.pairs >= 18); // a click every second master beat
        CHECK(r.worstMs < 1.0);
    }
    // 1.5 ms off (under the snap threshold): the rate nudge closes it in under a beat (0.47 s at 128).
    {
        const auto track = clickTrack(128.0, 30.0, 0.5);
        Rig rig;
        rig.load(0, *track, 0.5, 128.0);
        rig.load(1, *track, 0.5, 128.0);
        rig.engine.send(engine::Seek{1, 0.0015 * kRate});      // B 1.5 ms ahead
        rig.engine.send(engine::Play{0});
        rig.engine.send(engine::Play{1});
        rig.run(0.1, false);
        rig.engine.send(engine::SetSync{1, true});
        rig.run(0.01, false);
        CHECK(std::abs(rig.deck(1).phaseErrorMs + 1.5) < 0.1); // ahead = negative
        rig.run(60.0 / 128.0, false);                           // one beat later
        CHECK(std::abs(rig.deck(1).phaseErrorMs) < 0.05);
    }
    // 30 ms off when sync is pressed: one jump into phase.
    {
        const auto track = clickTrack(128.0, 30.0, 0.5);
        Rig rig;
        rig.load(0, *track, 0.5, 128.0);
        rig.load(1, *track, 0.5, 128.0);
        rig.engine.send(engine::Seek{1, 0.030 * kRate});
        rig.engine.send(engine::Play{0});
        rig.engine.send(engine::Play{1});
        rig.run(0.2, false);
        rig.engine.send(engine::SetSync{1, true});
        rig.run(0.02, false);
        CHECK(std::abs(rig.deck(1).phaseErrorMs) < 0.5); // the jump lands within a few samples...
        rig.run(60.0 / 128.0, false);
        CHECK(std::abs(rig.deck(1).phaseErrorMs) < 0.05); // ... and the nudge takes the rest within a beat
    }
    // Sync lock: the follower's fader moves the master (and so every synced deck); the master's moves the follower.
    {
        const auto trackA = clickTrack(128.0, 60.0, 0.5);
        const auto trackB = clickTrack(124.0, 60.0, 0.3);
        Rig rig;
        rig.load(0, *trackA, 0.5, 128.0);
        rig.load(1, *trackB, 0.3, 124.0);
        rig.engine.send(engine::SetSync{0, true});
        rig.engine.send(engine::SetSync{1, true});
        rig.engine.send(engine::Play{0});
        rig.engine.send(engine::Play{1});
        rig.run(2.0, false);
        CHECK(rig.deck(0).master);
        rig.engine.send(engine::SetTempo{1, 5.0}); // B: 124 x 1.05 = 130.2 BPM -> the master goes there
        rig.run(0.5, false);
        CHECK(std::abs(rig.engine.readState().masterBpm - 124.0 * 1.05) < 1e-6);
        CHECK(std::abs(rig.deck(0).tempoPercent - (124.0 * 1.05 / 128.0 - 1.0) * 100.0) < 1e-6);
        CHECK(std::abs(rig.deck(1).tempoPercent - 5.0) < 1e-6);
        rig.engine.send(engine::SetTempo{0, -2.0}); // the master's fader: the follower follows
        rig.run(0.5);
        CHECK(std::abs(rig.deck(1).tempoPercent - (128.0 * 0.98 / 124.0 - 1.0) * 100.0) < 1e-6);
        rig.run(10.0);
        CHECK(align(onsets(rig.a), onsets(rig.b), 3.5).worstMs < 1.0);
        // Sync off: the deck keeps its tempo and stops correcting.
        rig.engine.send(engine::SetSync{1, false});
        rig.run(0.1, false);
        CHECK_FALSE(rig.deck(1).synced);
        CHECK(rig.deck(1).phaseErrorMs == 0.0);
    }
    // A tempo the fader range doesn't reach: the range widens (100 -> 128 BPM needs +28 %).
    {
        const auto trackA = clickTrack(128.0, 10.0, 0.5);
        const auto trackB = clickTrack(100.0, 10.0, 0.5);
        Rig rig;
        rig.load(0, *trackA, 0.5, 128.0);
        rig.load(1, *trackB, 0.5, 100.0);
        rig.engine.send(engine::Play{0});
        rig.engine.send(engine::SetSync{1, true});
        rig.run(0.5, false);
        CHECK(rig.deck(1).tempoRangePercent == 50.0);
        CHECK(std::abs(rig.deck(1).tempoPercent - 28.0) < 1e-6);
    }
}
