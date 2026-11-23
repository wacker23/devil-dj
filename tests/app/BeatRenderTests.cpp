// P4-12: export. Done when: offline render bit-identical to real-time output for a deterministic pattern.

#include "app/BeatMakerProducer.h"
#include "app/BeatRender.h"
#include "beatmaker/DrumKits.h"
#include "beatmaker/SynthPresets.h"
#include "engine/AudioEngine.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <vector>

using namespace devildj;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

// Every step on a whole sample at 120 BPM (a 16th = 6,000): swing 0.5 (+1,500), micro-timing 20 % (+1,200),
// ratchets of 3 (2,000 apart); probability 100 everywhere; a pluck synth line; sends to the echo and reverb.
beatmaker::Project deterministic()
{
    auto p = beatmaker::defaultProject();
    p.swing = 0.5f;
    auto& a = p.patterns[0];
    a.drums = beatmaker::kitTracks(beatmaker::drumKits()[0]);
    for (int s = 0; s < 16; s += 4)
        a.drums[0].steps[static_cast<std::size_t>(s)] = {true, 120, 100, 0, 1}; // kick on the beats
    a.drums[1].steps[4] = {true, 110, 100, 20, 1};                           // snare, a touch late
    a.drums[1].steps[12] = {true, 110, 100, 0, 3};                           // snare roll
    for (int s = 1; s < 16; s += 2)
        a.drums[3].steps[static_cast<std::size_t>(s)] = {true, 90, 100, 0, 1}; // closed hats on the swung 16ths
    a.drums[4].steps[14] = {true, 100, 100, 0, 1};                         // open hat, choked by the next closed
    a.drums[1].reverbSend = 0.4f;
    a.drums[3].delaySend = 0.3f;
    a.synths.clear();
    beatmaker::SynthTrack bass;
    const auto& pluck = beatmaker::factorySynthPresets(dsp::SynthEngine::Subtractive)[5];
    bass.preset = beatmaker::synthPresetOf(pluck.name, pluck.params);
    bass.notes = {{0, 48, 36, 110}, {96, 24, 39, 100}, {192, 48, 43, 100}, {288, 72, 41, 100}};
    a.synths.push_back(bass);
    return p;
}
} // namespace

TEST_CASE("Export: the offline render is bit for bit what playback produces", "[app][beatmaker][export]")
{
    const auto project = deterministic();
    app::BeatRenderOptions o;
    o.passes = 2;
    o.tailSeconds = 0.0;
    o.masterBpm = 120.0;
    o.sampleRate = kRate;
    o.blockSize = kBlock;
    const auto offline = app::renderBeat(project, o);
    REQUIRE(offline.loopSamples == 2 * 96000);
    REQUIRE(offline.audio->numSamples() == offline.loopSamples);

    // Playback: a fresh engine set up the same way, the producer sending windows every ~25 ms of audio as in the app.
    auto engine = std::make_unique<engine::AudioEngine>();
    engine->prepare(kRate, kBlock);
    engine->send(engine::SetInternalTempo{120.0});
    engine->send(engine::SetClockSource{engine::MasterClock::Mode::Internal, engine::MasterClock::kInternal});
    for (int p = 0; p < beatmaker::kPatternCount; ++p)
    {
        const auto& pat = project.patterns[static_cast<std::size_t>(p)];
        for (std::size_t t = 0; t < pat.drums.size(); ++t)
            engine->send(engine::SetDrumSound{static_cast<std::uint16_t>(p * 16 + static_cast<int>(t)),
                                              beatmaker::engineDrumSound(pat.drums[t].sound)});
        for (std::size_t t = 0; t < pat.synths.size(); ++t)
            engine->send(engine::SetSynthSound{static_cast<std::uint16_t>(p * 16 + static_cast<int>(t)),
                                               beatmaker::synthParamsOf(pat.synths[t].preset)});
    }
    double seconds = 0.0;
    app::BeatMakerProducer producer([&](const engine::ScheduledEvent& e) { return engine->beatMakerEvents().tryPush(e); },
                                    false);
    producer.now = [&] { return seconds; };
    producer.setProject(std::make_shared<const beatmaker::Project>(project));
    producer.setClock({0, 0.0, 120.0, kRate, 0.0});
    producer.play(0, false); // from beat 0 = sample 0
    std::vector<float> live(static_cast<std::size_t>(2 * offline.loopSamples));
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[] = {l.data(), r.data()};
    for (int b = 0; b * kBlock < offline.loopSamples; ++b)
    {
        if (b % 5 == 0)
        {
            if (b > 0)
            {
                const auto& s = engine->readState();
                producer.setClock({s.sampleClock, s.masterBeat, s.masterBpm, s.sampleRate, seconds});
            }
            producer.tick();
        }
        engine->process(outs, 2, kBlock);
        seconds += kBlock / kRate;
        for (int i = 0; i < kBlock; ++i)
        {
            live[static_cast<std::size_t>(2 * (b * kBlock + i))] = l[static_cast<std::size_t>(i)];
            live[static_cast<std::size_t>(2 * (b * kBlock + i) + 1)] = r[static_cast<std::size_t>(i)];
        }
    }
    CHECK(engine->readState().beatLateEvents == 0);
    int different = 0, first = -1;
    double energy = 0.0;
    for (int i = 0; i < offline.loopSamples; ++i)
        for (int c = 0; c < 2; ++c)
        {
            const float a = offline.audio->audio.getSample(c, i), b = live[static_cast<std::size_t>(2 * i + c)];
            energy += static_cast<double>(a) * a;
            if (a != b)
            {
                ++different;
                if (first < 0)
                    first = i;
            }
        }
    INFO("first difference at sample " << first);
    CHECK(different == 0);
    CHECK(energy > 100.0); // it isn't silence that matches
}

TEST_CASE("Export: lengths, the tail, the song, its own tempo", "[app][beatmaker][export]")
{
    auto project = deterministic();
    app::BeatRenderOptions o;
    o.masterBpm = 128.0;
    o.tailSeconds = 1.5;
    auto r = app::renderBeat(project, o);
    CHECK(r.bpm == 128.0);
    CHECK(r.beats == 4.0);
    CHECK(r.loopSamples == 90000); // 4 beats at 128 BPM
    CHECK(r.audio->numSamples() == 90000 + 72000);
    CHECK(r.audio->sampleRate == 48000.0);
    // Something in the tail (the reverb / echo / the last notes), silence well after.
    float tail = 0.0f;
    for (int i = 90000; i < 100000; ++i)
        tail = std::max(tail, std::abs(r.audio->audio.getSample(0, i)));
    CHECK(tail > 1e-4f);
    // The song: A, A, B (B empty but 32 steps).
    project.patterns[1].lengthSteps = 32;
    beatmaker::setPatternLength(project.patterns[1], 32);
    project.song = {0, 0, 1};
    o.song = true;
    CHECK(app::renderBeats(project, o) == 16.0);
    // Its own tempo.
    project.followMaster = false;
    project.ownBpm = 100.0;
    o.song = false;
    o.tailSeconds = 0.0;
    o.passes = 4;
    r = app::renderBeat(project, o);
    CHECK(r.bpm == 100.0);
    CHECK(r.loopSamples == static_cast<int>(16.0 * 60.0 * 48000.0 / 100.0));
    CHECK(r.audio->numSamples() == r.loopSamples); // a seamless loop for the sampler
}
