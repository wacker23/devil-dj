#include "engine/AudioDeviceService.h"
#include "engine/AudioEngine.h"
#include "util/RealtimeGuard.h"

#include "FakeAudioDevice.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

using devildj::engine::AudioEngine;
using devildj::engine::SetTestTone;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

// Offline renderer: stereo buffers, blocks of kBlock samples, appended to 'left'/'right'.
struct Renderer
{
    explicit Renderer(AudioEngine& e)
        : engine(e)
        , blockL(kBlock)
        , blockR(kBlock)
    {
        engine.prepare(kRate, kBlock);
    }

    void render(int blocks)
    {
        for (int b = 0; b < blocks; ++b)
        {
            float* outs[] = {blockL.data(), blockR.data()};
            engine.process(outs, 2, kBlock);
            left.insert(left.end(), blockL.begin(), blockL.end());
            right.insert(right.end(), blockR.begin(), blockR.end());
        }
    }

    AudioEngine& engine;
    std::vector<float> blockL, blockR, left, right;
};

int blocksFor(double seconds)
{
    return static_cast<int>(std::ceil(seconds * kRate / kBlock));
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (auto i = from; i < to; ++i)
        sum += static_cast<double>(x[i]) * x[i];
    return std::sqrt(sum / static_cast<double>(to - from));
}

float maxStep(const std::vector<float>& x)
{
    float m = 0.0f;
    for (std::size_t i = 1; i < x.size(); ++i)
        m = std::max(m, std::abs(x[i] - x[i - 1]));
    return m;
}
} // namespace

TEST_CASE("AudioEngine outputs silence by default", "[engine][AudioEngine]")
{
    AudioEngine engine;
    Renderer r(engine);
    std::fill(r.blockL.begin(), r.blockL.end(), 0.5f); // garbage in the device buffer must be cleared
    r.render(10);

    CHECK(std::all_of(r.left.begin(), r.left.end(), [](float s) { return s == 0.0f; }));
    CHECK(std::all_of(r.right.begin(), r.right.end(), [](float s) { return s == 0.0f; }));

    const auto& state = engine.readState();
    CHECK(state.blockCounter == 10);
    CHECK(state.sampleRate == kRate);
    CHECK(state.blockSize == kBlock);
    CHECK_FALSE(state.testToneOn);
}

TEST_CASE("AudioEngine test tone command plays a sine at the requested pitch and level", "[engine][AudioEngine]")
{
    AudioEngine engine;
    Renderer r(engine);
    REQUIRE(engine.send(SetTestTone{true, 440.0f, -12.0f}));
    r.render(blocksFor(1.0));

    // Skip the 20 ms fade-in, then measure 0.5 s.
    const auto from = static_cast<std::size_t>(0.1 * kRate);
    const auto to = from + static_cast<std::size_t>(0.5 * kRate);
    const double amplitude = std::pow(10.0, -12.0 / 20.0);
    CHECK(std::abs(rms(r.left, from, to) - amplitude / std::sqrt(2.0)) < 1e-3);
    CHECK(r.left == r.right); // same tone on both channels

    int crossings = 0; // rising zero crossings in 0.5 s = 220 for 440 Hz
    for (auto i = from + 1; i < to; ++i)
        if (r.left[i - 1] < 0.0f && r.left[i] >= 0.0f)
            ++crossings;
    CHECK(std::abs(crossings - 220) <= 1);

    const auto& state = engine.readState();
    CHECK(state.testToneOn);
    CHECK(state.testToneHz == 440.0f);
    CHECK(state.commandsProcessed == 1);
}

TEST_CASE("AudioEngine test tone starts and stops without clicks", "[engine][AudioEngine]")
{
    AudioEngine engine;
    Renderer r(engine);
    const float amplitude = juce::Decibels::decibelsToGain(-6.0f);
    REQUIRE(engine.send(SetTestTone{true, 1000.0f, -6.0f}));
    r.render(blocksFor(0.2));
    REQUIRE(engine.send(SetTestTone{false, 1000.0f, -6.0f}));
    r.render(blocksFor(0.2));

    // A pure sine never moves more than A·2πf/fs per sample; a click would.
    const auto sineSlope = static_cast<float>(amplitude * 2.0 * std::numbers::pi * 1000.0 / kRate);
    CHECK(maxStep(r.left) <= sineSlope * 1.01f);

    // Fully silent again after the ramp.
    const auto tail = r.left.end() - kBlock;
    CHECK(std::all_of(tail, r.left.end(), [](float s) { return s == 0.0f; }));
    CHECK_FALSE(engine.readState().testToneOn);
}

TEST_CASE("AudioEngine measures CPU load and publishes it", "[engine][AudioEngine]")
{
    AudioEngine engine;
    Renderer r(engine);
    REQUIRE(engine.send(SetTestTone{true, 440.0f, -20.0f}));
    r.render(200);

    const auto& state = engine.readState();
    CHECK(state.cpuLoad > 0.0f);
    CHECK(state.cpuLoad < 0.5f); // a sine and a queue are far below the budget
    CHECK(state.cpuLoadPeak >= state.cpuLoad * 0.5f);
    CHECK(state.xruns == 0);
    CHECK(state.hostTimeSeconds > 0.0);
}

TEST_CASE("AudioEngine applies at most kMaxCommandsPerBlock commands per block", "[engine][AudioEngine]")
{
    AudioEngine engine;
    Renderer r(engine);
    for (int i = 0; i < 300; ++i)
        REQUIRE(engine.send(SetTestTone{false, 440.0f, -20.0f}));

    r.render(1);
    CHECK(engine.readState().commandsProcessed == AudioEngine::kMaxCommandsPerBlock);
    r.render(1);
    CHECK(engine.readState().commandsProcessed == 300);
}

TEST_CASE("AudioEngine reports a full command queue", "[engine][AudioEngine]")
{
    AudioEngine engine;
    engine.prepare(kRate, kBlock);
    for (std::size_t i = 0; i < AudioEngine::kCommandQueueSize; ++i)
        REQUIRE(engine.send(SetTestTone{}));
    CHECK_FALSE(engine.send(SetTestTone{}));
    CHECK(engine.droppedCommands() == 1);
}

TEST_CASE("AudioEngine's audio callback does not allocate", "[engine][AudioEngine]")
{
    if (!devildj::util::kRealtimeGuardEnabled)
        SKIP("RealtimeGuard is only active in Debug builds");

    AudioEngine engine;
    Renderer r(engine);
    r.render(1); // warm up (vectors in the test harness grow outside the callback)

    const auto before = devildj::util::realtimeViolationCount();
    std::vector<float> l(kBlock), rr(kBlock);
    float* outs[] = {l.data(), rr.data()};
    REQUIRE(engine.send(SetTestTone{true, 440.0f, -12.0f}));
    for (int i = 0; i < 100; ++i)
    {
        if (i == 50)
            REQUIRE(engine.send(SetTestTone{false, 440.0f, -12.0f}));
        engine.process(outs, 2, kBlock);
    }
    CHECK(devildj::util::realtimeViolationCount() == before);
}

TEST_CASE("AudioEngine runs as the device callback", "[engine][AudioEngine]")
{
    juce::ScopedJuceInitialiser_GUI juce;
    devildj::engine::AudioDeviceService service;
    service.addDeviceType(std::make_unique<devildj::test::FakeAudioDeviceType>("Fake", juce::StringArray{"Speakers"}));
    REQUIRE(service.initialise({}).isEmpty());
    REQUIRE(service.setSampleRate(44100.0).isEmpty());

    AudioEngine engine;
    service.setAudioCallback(&engine);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (engine.readState().blockCounter < 20 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    const auto& state = engine.readState();
    CHECK(state.blockCounter >= 20);
    CHECK(state.sampleRate == 44100.0); // prepare() was called from audioDeviceAboutToStart
    CHECK(state.blockSize == 256);

    service.setAudioCallback(nullptr);
}
