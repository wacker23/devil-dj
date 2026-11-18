// P1-03: each output layout verified with a test tone panned per bus.

#include "engine/AudioEngine.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using devildj::engine::AudioEngine;
using devildj::engine::Bus;
using devildj::engine::OutputLayout;
using devildj::engine::SetOutputLayout;
using devildj::engine::SetTestTone;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kBlocks = 40; // ~0.2 s

// Renders the engine into 'channels' device outputs and reports the RMS of each output after the fade-in.
std::vector<double> renderRms(AudioEngine& engine, int channels, int blockSize = kBlock)
{
    std::vector<std::vector<float>> out(static_cast<std::size_t>(channels), std::vector<float>(blockSize));
    std::vector<float*> ptrs;
    for (auto& ch : out)
        ptrs.push_back(ch.data());

    std::vector<double> sumSquares(static_cast<std::size_t>(channels), 0.0);
    std::size_t counted = 0;
    for (int b = 0; b < kBlocks; ++b)
    {
        engine.process(ptrs.data(), channels, blockSize);
        if (b < kBlocks / 2)
            continue; // skip the 20 ms ramp
        for (int ch = 0; ch < channels; ++ch)
            for (float s : out[static_cast<std::size_t>(ch)])
                sumSquares[static_cast<std::size_t>(ch)] += static_cast<double>(s) * s;
        counted += static_cast<std::size_t>(blockSize);
    }
    for (auto& v : sumSquares)
        v = std::sqrt(v / static_cast<double>(counted));
    return sumSquares;
}

// RMS of a full-scale-at-gainDb sine, scaled by 'factor'.
double sineRms(float gainDb, double factor = 1.0)
{
    return std::pow(10.0, gainDb / 20.0) * factor / std::sqrt(2.0);
}

constexpr float kGainDb = -12.0f;
constexpr double kTol = 2e-3;

AudioEngine& prepared(AudioEngine& e, OutputLayout layout)
{
    e.prepare(kRate, kBlock);
    e.send(SetOutputLayout{layout});
    return e;
}
} // namespace

TEST_CASE("Stereo layout: master tone panned left/right lands on outputs 1/2; cue is silent", "[engine][routing]")
{
    SECTION("master hard left")
    {
        AudioEngine e;
        prepared(e, OutputLayout::StereoMaster).send(SetTestTone{true, 440.0f, kGainDb, Bus::Master, -1.0f});
        const auto rms = renderRms(e, 2);
        CHECK(std::abs(rms[0] - sineRms(kGainDb)) < kTol);
        CHECK(rms[1] < 1e-6);
    }
    SECTION("master hard right")
    {
        AudioEngine e;
        prepared(e, OutputLayout::StereoMaster).send(SetTestTone{true, 440.0f, kGainDb, Bus::Master, 1.0f});
        const auto rms = renderRms(e, 2);
        CHECK(rms[0] < 1e-6);
        CHECK(std::abs(rms[1] - sineRms(kGainDb)) < kTol);
    }
    SECTION("cue is not routed")
    {
        AudioEngine e;
        prepared(e, OutputLayout::StereoMaster).send(SetTestTone{true, 440.0f, kGainDb, Bus::Cue, 0.0f});
        const auto rms = renderRms(e, 4);
        for (double v : rms)
            CHECK(v < 1e-6);
        CHECK_FALSE(e.readState().cueRouted);
    }
}

TEST_CASE("4-channel layout: master on 1/2, cue on 3/4, pan preserved", "[engine][routing]")
{
    SECTION("master hard left -> output 1 only")
    {
        AudioEngine e;
        prepared(e, OutputLayout::FourChannel).send(SetTestTone{true, 440.0f, kGainDb, Bus::Master, -1.0f});
        const auto rms = renderRms(e, 4);
        CHECK(std::abs(rms[0] - sineRms(kGainDb)) < kTol);
        CHECK(rms[1] < 1e-6);
        CHECK(rms[2] < 1e-6);
        CHECK(rms[3] < 1e-6);
    }
    SECTION("cue hard right -> output 4 only")
    {
        AudioEngine e;
        prepared(e, OutputLayout::FourChannel).send(SetTestTone{true, 440.0f, kGainDb, Bus::Cue, 1.0f});
        const auto rms = renderRms(e, 4);
        CHECK(rms[0] < 1e-6);
        CHECK(rms[1] < 1e-6);
        CHECK(rms[2] < 1e-6);
        CHECK(std::abs(rms[3] - sineRms(kGainDb)) < kTol);
        CHECK(e.readState().cueRouted);
        CHECK(e.readState().outputChannels == 4);
    }
    SECTION("on a stereo device the cue is dropped and reported")
    {
        AudioEngine e;
        prepared(e, OutputLayout::FourChannel).send(SetTestTone{true, 440.0f, kGainDb, Bus::Cue, 0.0f});
        const auto rms = renderRms(e, 2);
        CHECK(rms[0] < 1e-6);
        CHECK(rms[1] < 1e-6);
        CHECK_FALSE(e.readState().cueRouted);
    }
}

TEST_CASE("Split layout: master (mono) in the left ear, cue (mono) in the right", "[engine][routing]")
{
    SECTION("master tone, centred -> left only, summed to mono")
    {
        AudioEngine e;
        prepared(e, OutputLayout::SplitMono).send(SetTestTone{true, 440.0f, kGainDb, Bus::Master, 0.0f});
        const auto rms = renderRms(e, 2);
        // 0.5 * (L + R) with L = R = A * cos(pi/4)
        CHECK(std::abs(rms[0] - sineRms(kGainDb, std::cos(std::numbers::pi / 4.0))) < kTol);
        CHECK(rms[1] < 1e-6);
    }
    SECTION("master tone panned hard right still lands in the left ear")
    {
        AudioEngine e;
        prepared(e, OutputLayout::SplitMono).send(SetTestTone{true, 440.0f, kGainDb, Bus::Master, 1.0f});
        const auto rms = renderRms(e, 2);
        CHECK(std::abs(rms[0] - sineRms(kGainDb, 0.5)) < kTol);
        CHECK(rms[1] < 1e-6);
    }
    SECTION("cue tone -> right only")
    {
        AudioEngine e;
        prepared(e, OutputLayout::SplitMono).send(SetTestTone{true, 440.0f, kGainDb, Bus::Cue, -1.0f});
        const auto rms = renderRms(e, 2);
        CHECK(rms[0] < 1e-6);
        CHECK(std::abs(rms[1] - sineRms(kGainDb, 0.5)) < kTol);
        CHECK(e.readState().cueRouted);
        CHECK(e.readState().outputLayout == OutputLayout::SplitMono);
    }
}

TEST_CASE("Engine processes device blocks larger than prepared, in chunks", "[engine][routing]")
{
    AudioEngine e;
    e.prepare(kRate, 64); // device later delivers 256-sample blocks
    e.send(SetTestTone{true, 440.0f, kGainDb, Bus::Master, -1.0f});
    const auto rms = renderRms(e, 2, 256);
    CHECK(std::abs(rms[0] - sineRms(kGainDb)) < kTol);
    CHECK(rms[1] < 1e-6);
}

TEST_CASE("Unprepared engine stays silent", "[engine][routing]")
{
    AudioEngine e;
    e.send(SetTestTone{true, 440.0f, kGainDb, Bus::Master, 0.0f});
    std::vector<float> l(kBlock, 0.5f), r(kBlock, 0.5f);
    float* outs[] = {l.data(), r.data()};
    e.process(outs, 2, kBlock);
    CHECK(std::all_of(l.begin(), l.end(), [](float s) { return s == 0.0f; }));
}
