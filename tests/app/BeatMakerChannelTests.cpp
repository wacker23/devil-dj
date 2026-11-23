// P4-10: the Beat Maker's mixer channel. Done when: beat crossfades against a deck.

#include "app/BeatMakerProducer.h"
#include "beatmaker/DrumKits.h"
#include "beatmaker/Model.h"
#include "beatmaker/SynthPresets.h"
#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <vector>

using namespace devildj;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

// The engine with the producer driven by hand: the clock handed over every 25 ms of audio, "now" the audio time.
struct Rig
{
    engine::AudioEngine e;
    std::vector<engine::ScheduledEvent> pushed;
    double seconds = 0.0;
    std::unique_ptr<app::BeatMakerProducer> producer;
    std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
    int blocks = 0;

    Rig()
    {
        e.prepare(kRate, kBlock);
        producer = std::make_unique<app::BeatMakerProducer>(
            [this](const engine::ScheduledEvent& ev)
            {
                pushed.push_back(ev);
                return e.beatMakerEvents().tryPush(ev);
            },
            false);
        producer->now = [this] { return seconds; };
        e.send(engine::SetParam{engine::channelParam(engine::kBeatMakerChannel, engine::ChannelParam::Fader), 1.0f});
        render(1);
        handClock();
    }
    void handClock()
    {
        const auto& s = e.readState();
        producer->setClock({s.sampleClock, s.masterBeat, s.masterBpm, s.sampleRate, seconds});
    }
    // Renders `n` blocks; the producer ticks every ~25 ms. Returns the master output of the last block.
    void render(int n)
    {
        float* outs[] = {l.data(), r.data()};
        for (int i = 0; i < n; ++i)
        {
            if (producer != nullptr && blocks % 5 == 0) // 5 x 256 samples = 26.7 ms
            {
                handClock();
                producer->tick();
            }
            e.process(outs, 2, kBlock);
            seconds += kBlock / kRate;
            ++blocks;
        }
    }
};

// The level of one frequency in a block (Goertzel), as an amplitude.
double toneLevel(const std::vector<float>& x, double hz)
{
    const double w = 2.0 * 3.141592653589793 * hz / kRate, c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (const float v : x)
    {
        const double s0 = v + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / static_cast<double>(x.size());
}

// A sine track at `hz` (60 s, 120 BPM grid from 0).
struct SineTrack
{
    std::unique_ptr<engine::TrackBuffer> buffer = std::make_unique<engine::TrackBuffer>();
    explicit SineTrack(double hz)
    {
        buffer->sampleRate = kRate;
        const int n = static_cast<int>(60.0 * kRate);
        buffer->audio.setSize(2, n);
        for (int i = 0; i < n; ++i)
        {
            const auto v = static_cast<float>(0.4 * std::sin(2.0 * 3.141592653589793 * hz * i / kRate));
            buffer->audio.setSample(0, i, v);
            buffer->audio.setSample(1, i, v);
        }
    }
};

// One synth track holding a note through every bar: a steady tone (the voices' plain sine) at MIDI `pitch`.
beatmaker::Project holdTone(int pitch)
{
    beatmaker::Project p;
    beatmaker::SynthTrack s;
    s.notes = {{0, 16 * 24, static_cast<std::uint8_t>(pitch), 127}};
    p.patterns[0].synths.push_back(s);
    return p;
}
} // namespace

TEST_CASE("BEATS channel: the Beat Maker crossfades against a deck", "[app][beatmaker][channel]")
{
    Rig rig;
    auto& e = rig.e;
    SineTrack deckTone(300.0);
    e.send(engine::LoadTrack{0, deckTone.buffer.get(), 1});
    e.send(engine::SetBeatgrid{0, util::Beatgrid::constant(0.0, 120.0, kRate)});
    e.send(engine::SetParam{engine::channelParam(0, engine::ChannelParam::CrossfaderAssign), 0.0f}); // deck: A
    e.send(engine::SetParam{engine::channelParam(engine::kBeatMakerChannel, engine::ChannelParam::CrossfaderAssign), 2.0f}); // beats: B
    e.send(engine::Play{0});
    // C7 (2093 Hz) held by the Beat Maker, from the next bar.
    rig.producer->setProject(std::make_shared<const beatmaker::Project>(holdTone(96)));
    rig.producer->play(0, false);
    rig.render(static_cast<int>(2.5 * kRate / kBlock)); // past the first bar line
    REQUIRE(e.readState().beatVoices > 0);
    const auto measure = [&](float crossfader)
    {
        e.send(engine::SetParam{engine::ParamId::Crossfader, crossfader});
        rig.render(40); // settle
        std::vector<float> out;
        for (int i = 0; i < 16; ++i)
        {
            rig.render(1);
            out.insert(out.end(), rig.l.begin(), rig.l.end());
        }
        return std::pair{toneLevel(out, 300.0), toneLevel(out, 2093.0)};
    };
    const auto [deckA, beatsA] = measure(0.0f);
    const auto [deckMid, beatsMid] = measure(0.5f);
    const auto [deckB, beatsB] = measure(1.0f);
    WARN("deck / beats: A side " << deckA << " / " << beatsA << ", centre " << deckMid << " / " << beatsMid << ", B side "
                                 << deckB << " / " << beatsB);
    CHECK(deckA > 0.1);
    CHECK(beatsA < 0.01 * deckA); // all the deck
    CHECK(beatsB > 0.1);
    CHECK(deckB < 0.01 * beatsB); // all the beats
    CHECK(deckMid > 0.3 * deckA); // both in the middle
    CHECK(beatsMid > 0.3 * beatsB);
    CHECK(e.readState().beatLateEvents == 0);
}

TEST_CASE("BEATS channel: following the master, every 16th on its sample; its own tempo; stop", "[app][beatmaker][channel]")
{
    Rig rig;
    auto& e = rig.e;
    auto project = beatmaker::defaultProject();
    project.patterns[0].drums = beatmaker::kitTracks(beatmaker::drumKits()[0]);
    for (auto& s : project.patterns[0].drums[3].steps) // closed hat on every 16th
        s.on = true;
    rig.producer->setProject(std::make_shared<const beatmaker::Project>(project));
    rig.producer->play(0, false);
    rig.render(static_cast<int>(4.5 * kRate / kBlock));
    // The internal clock at 120 BPM from sample 0 (the first block was rendered before): a 16th = 6000 samples.
    REQUIRE(rig.pushed.size() > 10);
    const auto clock0 = rig.pushed.front().sampleTime;
    for (const auto& ev : rig.pushed)
        CHECK((ev.sampleTime - clock0) % 6000 == 0);
    CHECK(clock0 % 96000 == 0); // the first one on a bar line (4 beats = 96,000 samples)
    CHECK(e.readState().beatLateEvents == 0);
    CHECK(rig.producer->status().playing);
    CHECK(rig.producer->status().position.pattern == 0);
    // Its own tempo, 100 BPM: a 16th is 7,200 samples, the beat goes on from where it was.
    const double beatBefore = rig.producer->status().beat;
    project.followMaster = false;
    project.ownBpm = 100.0;
    rig.producer->setProject(std::make_shared<const beatmaker::Project>(project));
    CHECK(std::abs(rig.producer->status().beat - beatBefore) < 0.05);
    rig.pushed.clear();
    rig.render(static_cast<int>(3.0 * kRate / kBlock));
    REQUIRE(rig.pushed.size() > 10);
    std::vector<std::int64_t> gaps;
    for (std::size_t i = 1; i < rig.pushed.size(); ++i)
        gaps.push_back(rig.pushed[i].sampleTime - rig.pushed[i - 1].sampleTime);
    int sixteenths = 0;
    for (const auto g : gaps)
        sixteenths += g == 7200;
    CHECK(sixteenths >= static_cast<int>(gaps.size()) - 1); // the tempo change may leave one odd gap
    // Stop: nothing more is sent, what is queued goes, the voices fade.
    rig.producer->stop();
    e.send(engine::BeatMakerStop{});
    rig.pushed.clear();
    rig.render(20);
    CHECK(rig.pushed.empty());
    CHECK(e.readState().beatVoices == 0);
}

TEST_CASE("BEATS channel: drum and synth sounds set through the engine; an FX unit on it", "[app][beatmaker][channel]")
{
    Rig rig;
    auto& e = rig.e;
    e.send(engine::SetDrumSound{3, {dsp::DrumKind::Clap, {}}});
    e.send(engine::SetSynthSound{17, beatmaker::factorySynthPresets(dsp::SynthEngine::SubBass)[0].params});
    engine::ScheduledEvent hit;
    hit.kind = engine::VoiceKind::DrumSynth;
    hit.soundIndex = 3;
    hit.sampleTime = e.readState().sampleClock + 100;
    REQUIRE(e.beatMakerEvents().tryPush(hit));
    rig.render(2);
    CHECK(e.readState().beatVoices == 1);
    CHECK(e.readState().channels[engine::kBeatMakerChannel].peakL > 0.0f);
    CHECK(engine::kFxTargetCount == 8);
    CHECK(static_cast<int>(engine::FxTarget::BeatMaker) == 5);
}
