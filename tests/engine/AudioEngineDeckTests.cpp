// P1-06: transport through the engine — commands, published deck state, playback reaching the master bus,
// and a track swapped on a playing deck only after it has faded out.

#include "engine/AudioEngine.h"
#include "util/RealtimeGuard.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

using namespace devildj::engine;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

std::unique_ptr<TrackBuffer> constantTrack(float value, int samples = 48000)
{
    auto t = std::make_unique<TrackBuffer>();
    t->sampleRate = kRate;
    t->audio.setSize(2, samples);
    for (int ch = 0; ch < 2; ++ch)
        juce::FloatVectorOperations::fill(t->audio.getWritePointer(ch), value, samples);
    return t;
}

struct Out
{
    std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
};

void process(AudioEngine& e, Out& out)
{
    float* outs[] = {out.l.data(), out.r.data()};
    e.process(outs, 2, kBlock);
}

std::vector<BufferId> released(AudioEngine& e)
{
    std::vector<BufferId> ids;
    BufferId id = 0;
    while (e.popReleasedBuffer(id))
        ids.push_back(id);
    return ids;
}
} // namespace

TEST_CASE("Engine: play sends the deck to the master output; pause silences it", "[engine][transport]")
{
    const auto track = constantTrack(0.25f);
    AudioEngine e;
    e.prepare(kRate, kBlock);
    Out out;
    e.send(LoadTrack{0, track.get(), 7});
    process(e, out);
    CHECK(e.readState().decks[0].loaded);
    CHECK_FALSE(e.readState().decks[0].playing);
    CHECK(out.l.back() == 0.0f);

    e.send(Play{0});
    process(e, out);
    process(e, out);
    CHECK(e.readState().decks[0].playing);
    CHECK(out.l.back() == 0.25f); // stereo master = the track (no mixer yet)
    CHECK(out.r.back() == 0.25f);
    CHECK(e.readState().decks[0].positionSamples == 2.0 * kBlock);

    e.send(Pause{0});
    process(e, out);
    process(e, out);
    CHECK_FALSE(e.readState().decks[0].playing);
    CHECK(out.l.back() == 0.0f);
}

TEST_CASE("Engine: cue, toggle and seek commands reach the deck and its published state", "[engine][transport]")
{
    const auto track = constantTrack(0.1f);
    AudioEngine e;
    e.prepare(kRate, kBlock);
    Out out;
    e.send(LoadTrack{1, track.get(), 3});
    e.send(Seek{1, 12000.0});
    e.send(CuePress{1});
    process(e, out);
    const auto& s = e.readState().decks[1];
    CHECK(s.cuePreview);
    CHECK(s.playing);
    CHECK(s.cuePointSamples == 12000.0);

    e.send(CueRelease{1});
    process(e, out);
    CHECK_FALSE(e.readState().decks[1].playing);
    CHECK(e.readState().decks[1].positionSamples == 12000.0);

    e.send(TogglePlay{1});
    process(e, out);
    CHECK(e.readState().decks[1].playing);
    e.send(TogglePlay{1});
    process(e, out);
    CHECK_FALSE(e.readState().decks[1].playing);
    CHECK(e.readState().decks[1].lengthSamples == 48000);
}

TEST_CASE("Engine: loading onto a playing deck fades out first and releases the old buffer only then",
          "[engine][transport]")
{
    const auto first = constantTrack(0.5f);
    const auto second = constantTrack(-0.5f);
    AudioEngine e;
    e.prepare(kRate, 32); // small blocks so the 96-sample fade spans several of them
    std::vector<float> l(32), r(32);
    float* outs[] = {l.data(), r.data()};

    e.send(LoadTrack{0, first.get(), 1});
    e.send(Play{0});
    for (int i = 0; i < 10; ++i)
        e.process(outs, 2, 32);
    REQUIRE(e.readState().decks[0].playing);

    e.send(LoadTrack{0, second.get(), 2});
    e.process(outs, 2, 32);     // fading out the first track
    CHECK(released(e).empty()); // first buffer still being read: must not be released yet
    CHECK(e.readState().decks[0].bufferId == 1);
    CHECK(l.front() > 0.0f); // still the first track, fading

    for (int i = 0; i < 4; ++i) // past the 96-sample fade
        e.process(outs, 2, 32);
    CHECK(released(e) == std::vector<BufferId>{1});
    CHECK(e.readState().decks[0].bufferId == 2);
    CHECK_FALSE(e.readState().decks[0].playing); // a new track arrives paused at 0
    CHECK(e.readState().decks[0].positionSamples == 0.0);
}

TEST_CASE("Engine: loading onto a paused deck swaps immediately; unload releases", "[engine][transport]")
{
    const auto a = constantTrack(0.1f);
    const auto b = constantTrack(0.2f);
    AudioEngine e;
    e.prepare(kRate, kBlock);
    Out out;
    e.send(LoadTrack{0, a.get(), 10});
    e.send(LoadTrack{0, b.get(), 11});
    process(e, out);
    CHECK(released(e) == std::vector<BufferId>{10});
    CHECK(e.readState().decks[0].bufferId == 11);

    e.send(UnloadTrack{0});
    process(e, out);
    CHECK(released(e) == std::vector<BufferId>{11});
    CHECK_FALSE(e.readState().decks[0].loaded);
}

TEST_CASE("Engine: a track superseded while waiting for the fade is released without being used", "[engine][transport]")
{
    const auto a = constantTrack(0.1f), b = constantTrack(0.2f), c = constantTrack(0.3f);
    AudioEngine e;
    e.prepare(kRate, 16);
    std::vector<float> l(16), r(16);
    float* outs[] = {l.data(), r.data()};
    e.send(LoadTrack{0, a.get(), 1});
    e.send(Play{0});
    e.process(outs, 2, 16);
    e.send(LoadTrack{0, b.get(), 2}); // waits for the fade
    e.send(LoadTrack{0, c.get(), 3}); // replaces the pending one
    e.process(outs, 2, 16);
    CHECK(released(e) == std::vector<BufferId>{2});
    for (int i = 0; i < 10; ++i)
        e.process(outs, 2, 16);
    CHECK(released(e) == std::vector<BufferId>{1});
    CHECK(e.readState().decks[0].bufferId == 3);
}

TEST_CASE("Engine: two decks play together", "[engine][transport]")
{
    const auto a = constantTrack(0.25f), b = constantTrack(0.125f);
    AudioEngine e;
    e.prepare(kRate, kBlock);
    Out out;
    e.send(LoadTrack{0, a.get(), 1});
    e.send(LoadTrack{1, b.get(), 2});
    e.send(Play{0});
    e.send(Play{1});
    process(e, out);
    process(e, out);
    CHECK(out.l.back() == 0.375f);
}

TEST_CASE("Engine: playing, seeking and cueing a deck doesn't allocate on the audio thread", "[engine][transport]")
{
    if (!devildj::util::kRealtimeGuardEnabled)
        SKIP("RealtimeGuard is only active in Debug builds");

    const auto a = constantTrack(0.25f), b = constantTrack(0.5f);
    AudioEngine e;
    e.prepare(kRate, kBlock);
    Out out;
    process(e, out); // warm up

    const auto before = devildj::util::realtimeViolationCount();
    e.send(LoadTrack{0, a.get(), 1});
    e.send(Play{0});
    for (int i = 0; i < 50; ++i)
    {
        if (i == 10)
            e.send(Seek{0, 20000.0});
        if (i == 20)
            e.send(CuePress{0});
        if (i == 21)
            e.send(CueRelease{0});
        if (i == 30)
            e.send(TogglePlay{0});
        if (i == 40)
            e.send(LoadTrack{0, b.get(), 2});
        process(e, out);
    }
    CHECK(devildj::util::realtimeViolationCount() == before);
}
