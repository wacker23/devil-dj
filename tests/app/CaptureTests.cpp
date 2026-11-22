// P3-14: capture. Done when: captured loop plays back sample-identical (test).

#include "LoopExport.h"
#include "engine/AudioEngine.h"
#include "engine/Sampler.h"
#include "engine/TrackBuffer.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstring>

using namespace devildj;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

// Something that isn't periodic, so an off-by-one would show: a chirp with a little noise, different per channel.
std::unique_ptr<engine::TrackBuffer> music(double seconds)
{
    auto t = std::make_unique<engine::TrackBuffer>();
    t->sampleRate = kRate;
    const int n = static_cast<int>(seconds * kRate);
    t->audio.setSize(2, n);
    std::uint32_t seed = 99u;
    for (int i = 0; i < n; ++i)
    {
        const double time = i / kRate;
        seed = seed * 1664525u + 1013904223u;
        const double noise = (static_cast<double>(seed >> 8) / 8388608.0 - 1.0) * 0.05;
        t->audio.setSample(0, i, static_cast<float>(0.4 * std::sin(2.0 * 3.141592653589793 * (100.0 + 40.0 * time) * time) + noise));
        t->audio.setSample(1, i, static_cast<float>(0.4 * std::cos(2.0 * 3.141592653589793 * (150.0 + 30.0 * time) * time) - noise));
    }
    return t;
}
} // namespace

TEST_CASE("Capture: a deck's loop into a sampler slot plays back sample-identical; the WAV is bit-exact",
          "[app][capture][sampler]")
{
    auto track = music(20.0);
    // The deck's loop: beats 8..12 of a 128 BPM grid (not on whole-block boundaries).
    const auto grid = util::Beatgrid::constant(0.137 * kRate, 128.0, kRate);
    const auto start = static_cast<int>(std::llround(grid.sampleAt(8.0)));
    const auto end = static_cast<int>(std::llround(grid.sampleAt(12.0)));
    auto captured = app::copyRange(*track, start, end);
    REQUIRE(captured != nullptr);
    REQUIRE(captured->numSamples() == end - start);
    for (int c = 0; c < 2; ++c)
        CHECK(std::memcmp(captured->audio.getReadPointer(c), track->audio.getReadPointer(c) + start,
                          sizeof(float) * static_cast<std::size_t>(end - start)) == 0);
    CHECK(app::copyRange(*track, 500, 500) == nullptr);

    // Played back by the sampler (one-shot, gain 1, no sync): the same samples, after its 2 ms fade-in.
    engine::Sampler sampler;
    sampler.prepare(kRate, kBlock);
    sampler.load(0, captured.get(), 1, grid.bpmAt(start));
    sampler.press(0, 0);
    const int n = end - start;
    std::vector<float> l(static_cast<std::size_t>(n + kBlock)), r(l.size());
    for (int done = 0; done < n + kBlock; done += kBlock)
        sampler.render(l.data() + done, r.data() + done, std::min(kBlock, n + kBlock - done), 128.0);
    const int fade = static_cast<int>(std::lround(0.002 * kRate));
    CHECK(std::memcmp(l.data() + fade, track->audio.getReadPointer(0) + start + fade,
                      sizeof(float) * static_cast<std::size_t>(n - fade)) == 0);
    CHECK(std::memcmp(r.data() + fade, track->audio.getReadPointer(1) + start + fade,
                      sizeof(float) * static_cast<std::size_t>(n - fade)) == 0);
    CHECK(l[static_cast<std::size_t>(n)] == 0.0f); // and nothing after it

    // Saved as a 32-bit float WAV: read back, the very same samples.
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getChildFile("devildj-capture-" + juce::String(juce::Time::getHighResolutionTicks()) + ".wav");
    REQUIRE(app::writeLoopWav(*captured, 0, n, file, 32).isEmpty());
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    REQUIRE(reader != nullptr);
    CHECK(reader->bitsPerSample == 32);
    CHECK(reader->usesFloatingPointData);
    juce::AudioBuffer<float> back(2, n);
    reader->read(&back, 0, n, 0, true, true);
    for (int c = 0; c < 2; ++c)
        CHECK(std::memcmp(back.getReadPointer(c), captured->audio.getReadPointer(c), sizeof(float) * static_cast<std::size_t>(n)) == 0);
    reader.reset();
    file.deleteFile();
}

TEST_CASE("Capture: in the engine the captured loop, looped and synced at its own tempo, is the deck's loop",
          "[engine][capture][sampler]")
{
    // A loop captured at the track's tempo and played synced at that same tempo isn't stretched: the sampler
    // channel gives exactly the loop, round and round.
    auto track = music(10.0);
    auto captured = app::copyRange(*track, 48000, 48000 + 22500 * 4); // 4 beats at 128
    engine::Sampler sampler;
    sampler.prepare(kRate, kBlock);
    sampler.load(3, captured.get(), 1, 128.0);
    sampler.setMode(3, engine::Sampler::Mode::Loop);
    sampler.setSync(3, true);
    sampler.press(3, 0);
    const int n = 22500 * 4;
    std::vector<float> l(static_cast<std::size_t>(3 * n)), r(l.size());
    for (int done = 0; done < 3 * n; done += kBlock)
        sampler.render(l.data() + done, r.data() + done, std::min(kBlock, 3 * n - done), 128.0);
    // The second time round = the loop itself, exactly.
    CHECK(std::memcmp(l.data() + n, track->audio.getReadPointer(0) + 48000, sizeof(float) * static_cast<std::size_t>(n)) == 0);
    CHECK(std::memcmp(r.data() + 2 * n, track->audio.getReadPointer(1) + 48000, sizeof(float) * static_cast<std::size_t>(n)) == 0);
}
