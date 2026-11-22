// P3-03: loops. Done when: engine tests for loop wrap accuracy; no clicks on a sine test.

#include "LoopExport.h"
#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <vector>

using namespace devildj;
using engine::LoopOp;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

struct Rig
{
    engine::TrackBuffer track;
    engine::AudioEngine engine;
    float l[kBlock], r[kBlock];
    float* outs[2] = {l, r};
    util::Beatgrid grid = util::Beatgrid::constant(0.5 * kRate, 120.0, kRate); // a beat every 0.5 s
    explicit Rig(double hz = 0.0, double seconds = 60.0)
    {
        track.sampleRate = kRate;
        track.audio.setSize(2, static_cast<int>(seconds * kRate));
        for (int i = 0; i < track.audio.getNumSamples(); ++i)
        {
            const auto v = hz > 0.0 ? static_cast<float>(0.5 * std::sin(2.0 * 3.141592653589793 * hz * i / kRate)) : 0.0f;
            track.audio.setSample(0, i, v);
            track.audio.setSample(1, i, v);
        }
        engine.prepare(kRate, kBlock);
        engine.send(engine::LoadTrack{0, &track, 1});
        engine.send(engine::SetBeatgrid{0, grid});
        block();
    }
    void block() { engine.process(outs, 2, kBlock); }
    void blocks(int n)
    {
        for (int i = 0; i < n; ++i)
            block();
    }
    void loop(LoopOp op, double beats = 0.0)
    {
        engine.send(engine::Loop{0, op, beats});
        block();
    }
    const engine::DeckState& deck() { return engine.readState().decks[0]; }
};
} // namespace

TEST_CASE("Loops: the playhead wraps at the exact sample, the overshoot carried (rate 1, +3.7 %, key lock)",
          "[engine][loop]")
{
    for (const auto [tempo, keyLock] : {std::pair{0.0, false}, std::pair{3.7, false}, std::pair{-2.5, true}})
    {
        INFO("tempo " << tempo << " %, key lock " << keyLock);
        Rig rig;
        rig.engine.send(engine::SetTempo{0, tempo});
        rig.engine.send(engine::SetKeyLock{0, keyLock});
        const double start = 1000.3, end = 25000.7; // not on whole samples, not a multiple of the block
        rig.engine.send(engine::Seek{0, 900.0});
        rig.engine.send(engine::Loop{0, LoopOp::Set, 0.0, start, end});
        rig.engine.send(engine::Play{0});
        rig.block();
        // The model: forward by the rate each sample; at the end, back to start + what went past it.
        const double rate = 1.0 + tempo / 100.0;
        double expected = start + kBlock * rate;
        CHECK(std::abs(rig.deck().positionSamples - expected) < 1e-6);
        int wraps = 0;
        double worst = 0.0;
        for (int b = 0; b < 600; ++b)
        {
            rig.block();
            for (int i = 0; i < kBlock; ++i)
            {
                expected += rate;
                if (expected >= end)
                {
                    expected = start + (expected - end);
                    ++wraps;
                }
            }
            worst = std::max(worst, std::abs(rig.deck().positionSamples - expected));
        }
        WARN("tempo " << tempo << " %, key lock " << keyLock << ": " << wraps << " wraps, worst position error "
                      << worst << " samples");
        CHECK(wraps >= 5);
        CHECK(worst < 1e-6);
        CHECK(rig.deck().loopActive);
    }
}

TEST_CASE("Loops: no clicks at the wrap (sine)", "[engine][loop]")
{
    // A 375 Hz sine (128 samples a period): a loop of whole periods is seamless; any other length crossfades in
    // 2 ms, so the wave never jumps - the largest sample-to-sample step stays near the sine's own.
    for (const double length : {128.0 * 300.0, 12345.6})
    {
        INFO("loop of " << length << " samples");
        Rig rig(375.0, 20.0);
        rig.engine.send(engine::Loop{0, LoopOp::Set, 0.0, 4800.0, 4800.0 + length});
        rig.engine.send(engine::Play{0});
        rig.blocks(4); // past the fade-in
        const double slope = 2.0 * 3.141592653589793 * 375.0 / kRate * 0.5; // the sine's largest step
        double worst = 0.0;
        float previous = rig.l[kBlock - 1];
        for (int b = 0; b < 500; ++b)
        {
            rig.block();
            for (int i = 0; i < kBlock; ++i)
            {
                worst = std::max(worst, static_cast<double>(std::abs(rig.l[i] - previous)));
                previous = rig.l[i];
            }
        }
        WARN("loop of " << length << " samples: largest step " << worst / slope << " x the sine's own");
        CHECK(worst < (length == 128.0 * 300.0 ? 1.02 : 1.6) * slope);
    }
}

TEST_CASE("Loops: auto loop on the grid, in / out, halve / double, reloop, exit, move, seek out", "[engine][loop]")
{
    Rig rig;
    rig.engine.send(engine::SetQuantize{true, 1.0f});
    rig.engine.send(engine::Seek{0, rig.grid.sampleAt(10.4)});
    rig.engine.send(engine::Play{0});
    rig.block();
    // Auto loop (4 beats to start): from the beat at or before the playhead (10), 4 beats.
    rig.loop(LoopOp::Auto);
    REQUIRE(rig.deck().loopActive);
    CHECK(std::abs(rig.deck().loopStart - rig.grid.sampleAt(10.0)) < 1e-6);
    CHECK(std::abs(rig.deck().loopEnd - rig.grid.sampleAt(14.0)) < 1e-6);
    CHECK(std::abs(rig.deck().loopBeats - 4.0) < 1e-9);
    // Halve / double the loop.
    rig.loop(LoopOp::Halve);
    CHECK(std::abs(rig.deck().loopEnd - rig.grid.sampleAt(12.0)) < 1e-6);
    rig.loop(LoopOp::Double);
    rig.loop(LoopOp::Double);
    CHECK(std::abs(rig.deck().loopBeats - 8.0) < 1e-9);
    // Beat jump while looping moves the loop, and the playhead with it.
    const double into = rig.deck().positionSamples - rig.deck().loopStart;
    rig.engine.send(engine::BeatJump{0, 16.0});
    rig.block();
    CHECK(std::abs(rig.deck().loopStart - rig.grid.sampleAt(26.0)) < 1e-6);
    CHECK(std::abs(rig.deck().loopEnd - rig.grid.sampleAt(34.0)) < 1e-6);
    CHECK(std::abs(rig.deck().positionSamples - (rig.grid.sampleAt(26.0) + into + kBlock)) < 2.0);
    // Auto loop again = exit; halve / double then change the auto-loop size; reloop brings the last loop back.
    rig.loop(LoopOp::Auto);
    CHECK_FALSE(rig.deck().loopActive);
    rig.loop(LoopOp::Halve);
    CHECK(rig.deck().autoLoopBeats == 2.0);
    for (int i = 0; i < 20; ++i)
        rig.loop(LoopOp::Halve);
    CHECK(rig.deck().autoLoopBeats == 1.0 / 64.0); // the smallest
    rig.loop(LoopOp::Reloop);
    CHECK(rig.deck().loopActive);
    CHECK(std::abs(rig.deck().loopStart - rig.grid.sampleAt(26.0)) < 1e-6);
    CHECK(rig.deck().positionSamples < rig.grid.sampleAt(26.0) + 2 * kBlock);
    // Exit: playback goes on past the end.
    rig.loop(LoopOp::Exit);
    CHECK_FALSE(rig.deck().loopActive);
    CHECK(rig.deck().playing);
    // Manual in / out (quantize: the in point on the beat before, the out point on the nearest beat).
    rig.engine.send(engine::SetQuantize{false, 1.0f});
    rig.engine.send(engine::Seek{0, rig.grid.sampleAt(40.0) + 1000.0});
    rig.loop(LoopOp::In);
    CHECK(rig.deck().loopInSet);
    rig.blocks(300); // ~1.6 s later
    rig.loop(LoopOp::Out);
    REQUIRE(rig.deck().loopActive);
    CHECK_FALSE(rig.deck().loopInSet);
    CHECK(std::abs(rig.deck().loopStart - (rig.grid.sampleAt(40.0) + 1000.0)) < 1e-6); // seek and in, same block
    // A seek out of the loop ends it.
    rig.engine.send(engine::Seek{0, 5.0 * kRate});
    rig.block();
    CHECK_FALSE(rig.deck().loopActive);
    // No grid: no auto loop (manual loops still work).
    rig.engine.send(engine::SetBeatgrid{0, util::Beatgrid()});
    rig.loop(LoopOp::Auto);
    CHECK_FALSE(rig.deck().loopActive);
}

TEST_CASE("Loops: a pad stores the playing loop; a hot loop loops (quantized: once its jump is there)",
          "[engine][loop][hotcue]")
{
    Rig rig;
    rig.engine.send(engine::Seek{0, rig.grid.sampleAt(20.0)});
    rig.engine.send(engine::Play{0});
    rig.loop(LoopOp::Auto); // beats 20..24
    rig.engine.send(engine::HotCue{0, 4, engine::HotCueOp::Press}); // empty pad E: stores the loop
    rig.block();
    CHECK(std::abs(rig.deck().hotCueStart[4] - rig.grid.sampleAt(20.0)) < 1e-6);
    CHECK(std::abs(rig.deck().hotCueLength[4] - (rig.grid.sampleAt(24.0) - rig.grid.sampleAt(20.0))) < 1e-6);
    rig.loop(LoopOp::Exit);
    rig.engine.send(engine::Seek{0, rig.grid.sampleAt(50.3)});
    rig.engine.send(engine::SetQuantize{true, 1.0f});
    rig.block();
    // Quantized: still no loop until the next beat; then the deck loops 20..24.
    rig.engine.send(engine::HotCue{0, 4, engine::HotCueOp::Press});
    rig.block();
    CHECK_FALSE(rig.deck().loopActive);
    rig.blocks(120);
    REQUIRE(rig.deck().loopActive);
    CHECK(std::abs(rig.deck().loopStart - rig.grid.sampleAt(20.0)) < 1e-6);
    CHECK(rig.deck().positionSamples >= rig.grid.sampleAt(20.0));
    CHECK(rig.deck().positionSamples < rig.grid.sampleAt(24.0));
}

TEST_CASE("Loops: export as WAV", "[app][loop]")
{
    Rig rig(440.0, 10.0);
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("devildj-loop-" + juce::String(juce::Time::getHighResolutionTicks()) + ".wav");
    CHECK(app::writeLoopWav(rig.track, 48000, 48000 + 96000, file).isEmpty());
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    REQUIRE(reader != nullptr);
    CHECK(reader->lengthInSamples == 96000);
    CHECK(reader->numChannels == 2);
    CHECK(reader->sampleRate == 48000.0);
    CHECK(reader->bitsPerSample == 24);
    juce::AudioBuffer<float> back(2, 96000);
    reader->read(&back, 0, 96000, 0, true, true);
    CHECK(std::abs(back.getSample(0, 100) - rig.track.audio.getSample(0, 48100)) < 1e-5f);
    reader.reset();
    file.deleteFile();
    CHECK_FALSE(app::writeLoopWav(rig.track, 500, 500, file).isEmpty()); // empty loop
}
