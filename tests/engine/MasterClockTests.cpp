// P2-18: the master clock. Done when: unit tests for phase continuity across tempo changes.

#include "engine/AudioEngine.h"
#include "engine/MasterClock.h"
#include "engine/TrackBuffer.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <random>

using devildj::engine::ClockDeckInput;
using devildj::engine::MasterClock;
using Mode = MasterClock::Mode;

namespace
{
constexpr double kRate = 48000.0;

bool near(double a, double b, double tolerance = 1e-9)
{
    return std::abs(a - b) <= tolerance;
}

// A deck playing a constant-tempo grid: its beat after `samples` at `bpm`.
struct FakeDeck
{
    double beat = 0.0, bpm = 0.0;
    bool playing = false, hasGrid = true, synced = true;
    ClockDeckInput input() const { return {playing, hasGrid, synced, beat, bpm}; }
    void advance(int samples)
    {
        if (playing)
            beat += samples * bpm / (60.0 * kRate);
    }
};
} // namespace

TEST_CASE("Master clock: the internal tempo, beat <-> sample", "[engine][clock]")
{
    MasterClock clock;
    clock.prepare(kRate);
    clock.reset();
    CHECK(clock.bpm() == 120.0);
    CHECK(clock.sourceDeck() == MasterClock::kInternal);
    const std::array<ClockDeckInput, 2> none{};
    for (int i = 0; i < 100; ++i)
        clock.beginBlock(480, none); // 100 x 10 ms = 1 s = 2 beats at 120
    CHECK(clock.blockStart() == 99 * 480);
    CHECK(clock.blockEnd() == 48000);
    CHECK(near(clock.beatAtBlockEnd(), 2.0));
    CHECK(near(clock.beatAt(24000.0), 1.0));
    CHECK(near(clock.sampleAt(1.0), 24000.0, 1e-6));
    CHECK(near(clock.sampleAt(clock.beatAt(12345.0)), 12345.0, 1e-6));
    CHECK(near(clock.beatsPerSample(), 2.0 / 48000.0));

    clock.setInternalBpm(1000.0);
    CHECK(clock.internalBpm() == MasterClock::kMaxBpm);
    clock.setInternalBpm(1.0);
    CHECK(clock.internalBpm() == MasterClock::kMinBpm);
    clock.setInternalBpm(std::nan(""));
    CHECK(clock.internalBpm() == MasterClock::kMinBpm);
}

TEST_CASE("Master clock: phase continuity across tempo changes (10 minutes, random blocks and tempos)",
          "[engine][clock]")
{
    MasterClock clock;
    clock.prepare(kRate);
    clock.reset();
    const std::array<ClockDeckInput, 2> none{};
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> blockSize(1, 2048);
    std::uniform_real_distribution<double> tempo(60.0, 200.0);
    std::uniform_int_distribution<int> change(0, 30);

    // The exact beat: the integral of the tempo over the samples, block by block.
    double expected = 0.0, worstJump = 0.0;
    std::int64_t samples = 0;
    int changes = 0;
    double previousEnd = 0.0;
    while (samples < static_cast<std::int64_t>(600.0 * kRate))
    {
        if (change(rng) == 0)
        {
            clock.setInternalBpm(tempo(rng));
            ++changes;
        }
        const int n = blockSize(rng);
        clock.beginBlock(n, none);
        // The new block starts exactly where the last one ended, whatever the tempo did.
        worstJump = std::max(worstJump, std::abs(clock.beatAtBlockStart() - previousEnd));
        CHECK(clock.blockStart() == samples);
        expected += n * clock.internalBpm() / (60.0 * kRate);
        samples += n;
        previousEnd = clock.beatAtBlockEnd();
    }
    INFO(changes << " tempo changes, " << samples << " samples");
    CHECK(changes > 100);
    CHECK(worstJump == 0.0);
    CHECK(near(clock.beatAtBlockEnd(), expected, 1e-7)); // ~1,000 beats: no drift beyond rounding
    CHECK(clock.blockEnd() == samples);
}

TEST_CASE("Master clock: a deck leads - its phase, whole bars kept, tempo changes, stop and restart",
          "[engine][clock]")
{
    MasterClock clock;
    clock.prepare(kRate);
    clock.reset();
    std::array<FakeDeck, 2> decks;
    const auto inputs = [&] { return std::array<ClockDeckInput, 2>{decks[0].input(), decks[1].input()}; };
    const auto run = [&](int blocks, int n = 512)
    {
        for (int i = 0; i < blocks; ++i)
        {
            clock.beginBlock(n, inputs());
            for (auto& d : decks)
                d.advance(n);
        }
    };

    // Internal at 120 for 10 s: beat 20.
    run(static_cast<int>(10.0 * kRate / 512));
    const double before = clock.beatAtBlockEnd();
    CHECK(near(before, 20.0, 0.03));

    // Deck B starts playing at its beat 33.25 (128 BPM): it leads (Auto); the clock takes its phase (.25) and the
    // whole bars nearest to where it was (33.25 - 12 = 21.25, within 2 beats of 20).
    decks[1] = {33.25, 128.0, true};
    run(1);
    CHECK(clock.sourceDeck() == 1);
    CHECK(clock.bpm() == 128.0);
    const double offset = clock.beatAtBlockStart() - 33.25;
    CHECK(std::abs(std::remainder(offset, 4.0)) < 1e-9); // whole bars
    CHECK(std::abs(clock.beatAtBlockStart() - before) <= 2.0);

    // Following: always the deck's beat + the bar offset, through a tempo change on the deck (+6 %) - and the beat
    // runs on without a jump between blocks, as the deck's does.
    double worstJump = 0.0;
    for (int i = 0; i < 2000; ++i)
    {
        if (i == 1000)
            decks[1].bpm = 128.0 * 1.06;
        const double end = clock.blockEnd() > 0 ? clock.beatAtBlockEnd() : 0.0;
        clock.beginBlock(512, inputs());
        worstJump = std::max(worstJump, std::abs(clock.beatAtBlockStart() - end));
        CHECK(near(clock.beatAtBlockStart(), decks[1].beat + offset, 1e-9));
        for (auto& d : decks)
            d.advance(512);
    }
    CHECK(worstJump < 1e-9);
    CHECK(near(clock.bpm(), 128.0 * 1.06));

    // The deck stops: the clock runs on from where it was at the deck's last tempo (no jump).
    const double atStop = clock.beatAtBlockEnd();
    decks[1].playing = false;
    clock.beginBlock(512, inputs());
    CHECK(clock.sourceDeck() == MasterClock::kInternal);
    CHECK(near(clock.beatAtBlockStart(), atStop, 1e-9));
    CHECK(near(clock.bpm(), 128.0 * 1.06));
    CHECK(near(clock.internalBpm(), 128.0 * 1.06));
    run(100);

    // It starts again (from a cue): it leads again, its phase again, bars kept nearest.
    decks[1].playing = true;
    decks[1].beat = 64.0;
    const double running = clock.beatAtBlockEnd();
    run(1);
    CHECK(clock.sourceDeck() == 1);
    CHECK(std::abs(std::remainder(clock.beatAtBlockStart() - 64.0, 4.0)) < 1e-9);
    CHECK(std::abs(clock.beatAtBlockStart() - running) <= 2.0);
}

TEST_CASE("Master clock: who leads (Auto, a chosen deck, internal)", "[engine][clock]")
{
    MasterClock clock;
    clock.prepare(kRate);
    clock.reset();
    std::array<ClockDeckInput, 3> d{};
    const auto lead = [&]
    {
        clock.beginBlock(256, d);
        return clock.sourceDeck();
    };
    CHECK(lead() == MasterClock::kInternal); // nothing plays
    d[2] = {true, true, true, 10.0, 125.0};
    CHECK(lead() == 2);
    d[0] = {true, true, true, 5.0, 130.0};
    CHECK(lead() == 2); // Auto stays with the deck that leads
    d[2].playing = false;
    CHECK(lead() == 0); // ... until it stops: the first other one
    d[1] = {true, false, true, 0.0, 0.0}; // playing without a grid: can't lead
    d[0].bpm = 1000.0;                    // nonsense tempo: can't lead
    CHECK(lead() == MasterClock::kInternal);
    d[0].bpm = 130.0;
    CHECK(lead() == 0);
    // A synced playing deck is preferred over one without SYNC; with none synced, any playing deck leads (a synced
    // deck needs something to follow).
    d[0].synced = false;
    d[2] = {true, true, true, 10.0, 125.0};
    clock.setMode(Mode::Internal);
    lead();
    clock.setMode(Mode::Auto);
    CHECK(lead() == 2);
    d[2].synced = false;
    clock.setMode(Mode::Internal);
    lead();
    clock.setMode(Mode::Auto);
    CHECK(lead() == 0);
    d[0].synced = true;
    d[2] = {false, true, true, 10.0, 125.0};

    clock.setMode(Mode::Deck, 2);
    CHECK(lead() == MasterClock::kInternal); // chosen deck B... C is stopped
    d[2].playing = true;
    CHECK(lead() == 2);
    clock.setMode(Mode::Deck, 7); // no such deck
    CHECK(lead() == MasterClock::kInternal);
    clock.setMode(Mode::Internal);
    clock.setInternalBpm(100.0);
    CHECK(lead() == MasterClock::kInternal);
    CHECK(clock.bpm() == 100.0);
    clock.setMode(Mode::Auto);
    CHECK(lead() == 0);
}

TEST_CASE("Master clock in the engine: follows the playing deck's grid and tempo; commands; sample clock",
          "[engine][clock]")
{
    namespace engine = devildj::engine;
    engine::TrackBuffer track;
    track.sampleRate = kRate;
    track.audio.setSize(2, static_cast<int>(60.0 * kRate));
    track.audio.clear();
    engine::AudioEngine e;
    e.prepare(kRate, 256);
    float l[256], r[256];
    float* outs[] = {l, r};
    const auto blocks = [&](int n)
    {
        for (int i = 0; i < n; ++i)
            e.process(outs, 2, 256);
    };

    blocks(10);
    CHECK(e.readState().sampleClock == 2560);
    CHECK(e.readState().masterSource == -1);
    e.send(engine::SetInternalTempo{100.0});
    blocks(1);
    CHECK(e.readState().masterBpm == 100.0);

    const auto grid = devildj::util::Beatgrid::constant(0.5 * kRate, 124.0, kRate);
    e.send(engine::LoadTrack{0, &track, 1});
    e.send(engine::SetBeatgrid{0, grid});
    e.send(engine::Play{0});
    blocks(int(5.0 * kRate / 256));
    auto s = e.readState();
    CHECK(s.masterSource == 0);
    CHECK(near(s.masterBpm, 124.0));
    // The clock's beat is the deck's grid beat (plus whole bars), its phase the same.
    const double deckBeat = grid.beatAt(s.decks[0].positionSamples);
    CHECK(std::abs(std::remainder(s.masterBeat - deckBeat, 4.0)) < 1e-6);

    // Tempo +6 %: the clock's tempo follows.
    e.send(engine::SetTempo{0, 6.0});
    blocks(4);
    CHECK(near(e.readState().masterBpm, 124.0 * 1.06, 1e-6));

    // Internal only: the deck no longer leads, the tempo runs on at the deck's last.
    e.send(engine::SetClockSource{Mode::Internal});
    blocks(2);
    s = e.readState();
    CHECK(s.masterSource == -1);
    CHECK(s.clockMode == static_cast<std::uint8_t>(Mode::Internal));
    CHECK(near(s.masterBpm, 124.0 * 1.06, 1e-6));
    CHECK(near(e.masterClock().bpm(), s.masterBpm));
    e.send(engine::SetClockSource{Mode::Deck, 0});
    blocks(1);
    CHECK(e.readState().masterSource == 0);
}
