// P5-12: the code deck's mixer channel. Done when: the code deck crossfades against a track.

#include "app/CodeDeckProducer.h"
#include "app/CodeSoundBank.h"
#include "codebeat/BigStack.h"
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

// The engine with the code deck's producer driven by hand (on this thread): the clock handed over every ~25 ms of
// audio, "now" the audio time, new sounds installed the way the app does it.
struct Rig
{
    juce::File folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("devildj-code", "");
    engine::AudioEngine e;
    double seconds = 0.0;
    std::unique_ptr<app::CodeDeckProducer> producer;
    std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
    int blocks = 0;

    Rig()
    {
        folder.createDirectory();
        e.prepare(kRate, kBlock);
        producer = std::make_unique<app::CodeDeckProducer>(
            [this](const engine::ScheduledEvent& ev) { return e.codeDeckEvents().tryPush(ev); }, false);
        producer->now = [this] { return seconds; };
        producer->setSoundBank(std::make_shared<app::CodeSoundBank>(folder));
        e.send(engine::SetParam{engine::channelParam(engine::kCodeDeckChannel, engine::ChannelParam::Fader), 1.0f});
        render(1);
    }
    ~Rig() { folder.deleteRecursively(); }

    void render(int n)
    {
        float* outs[] = {l.data(), r.data()};
        for (int i = 0; i < n; ++i)
        {
            if (blocks % 5 == 0) // 5 x 256 samples = 26.7 ms
            {
                const auto& s = e.readState();
                producer->setClock({s.sampleClock, s.masterBeat, s.masterBpm, s.sampleRate, seconds});
                producer->tick();
                for (const auto& [index, sound] : producer->takeNewSounds())
                {
                    if (sound.kind == app::CodeSound::Kind::Drum)
                        e.send(engine::SetDrumSound{index, sound.drum, engine::SourceId::CodeDeck});
                    else if (sound.kind == app::CodeSound::Kind::Synth)
                        e.send(engine::SetSynthSound{index, sound.synth, engine::SourceId::CodeDeck});
                }
            }
            e.process(outs, 2, kBlock);
            seconds += kBlock / kRate;
            ++blocks;
        }
    }
};

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

struct Levels
{
    double deck = 0.0, code = 0.0;
};
} // namespace

TEST_CASE("CODE channel: the code deck crossfades against a track", "[app][codebeat][channel]")
{
    Levels atA, atMid, atB;
    int codeVoices = 0;
    std::uint64_t late = 0, producerLate = 0, unknown = 0;
    std::string error;
    testing::onBigStack([&]
    {
        Rig rig;
        auto& e = rig.e;
        SineTrack deckTone(300.0);
        e.send(engine::LoadTrack{0, deckTone.buffer.get(), 1});
        e.send(engine::SetBeatgrid{0, util::Beatgrid::constant(0.0, 120.0, kRate)});
        e.send(engine::SetParam{engine::channelParam(0, engine::ChannelParam::CrossfaderAssign), 0.0f});                    // deck: A
        e.send(engine::SetParam{engine::channelParam(engine::kCodeDeckChannel, engine::ChannelParam::CrossfaderAssign), 2.0f}); // code: B
        e.send(engine::Play{0});
        // C7 (2093 Hz) held by a sine synth, a note every cycle lasting the whole cycle.
        rig.producer->submit(R"(note("c7").s("sine").legato(1.05))");
        // The code started just after the cycle line at 0: its first note is at the next one (2 s at 120 BPM).
        rig.render(static_cast<int>(2.5 * kRate / kBlock));
        codeVoices = e.readState().codeVoices;
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
            return Levels{toneLevel(out, 300.0), toneLevel(out, 2093.0)};
        };
        atA = measure(0.0f);
        atMid = measure(0.5f);
        atB = measure(1.0f);
        late = e.readState().codeLateEvents;
        const auto st = rig.producer->status();
        producerLate = st.late;
        unknown = st.unknownSounds;
        error = st.lastError;
    });
    INFO(error);
    WARN("deck / code: A side " << atA.deck << " / " << atA.code << ", centre " << atMid.deck << " / " << atMid.code
                                << ", B side " << atB.deck << " / " << atB.code);
    CHECK(codeVoices > 0);
    CHECK(atA.deck > 0.1);
    CHECK(atA.code < 0.01 * atA.deck); // all the deck
    CHECK(atB.code > 0.05);
    CHECK(atB.deck < 0.01 * atB.code); // all the code deck
    CHECK(atMid.deck > 0.3 * atA.deck); // both in the middle
    CHECK(atMid.code > 0.3 * atB.code);
    CHECK(late == 0);
    CHECK(producerLate == 0);
    CHECK(unknown == 0);
}

TEST_CASE("CODE channel: its own sound tables, FX target, cue, and a stop that releases its voices", "[app][codebeat][channel]")
{
    int voicesPlaying = 0, voicesAfterStop = -1, beatVoices = -1;
    float codePeak = 0.0f, beatPeak = -1.0f;
    bool cueOn = false;
    testing::onBigStack([&]
    {
        Rig rig;
        auto& e = rig.e;
        // A kick from the code deck: on the CODE channel only, not on the Beat Maker's.
        rig.producer->submit(R"(s("bd*4"))");
        rig.render(static_cast<int>(1.0 * kRate / kBlock));
        voicesPlaying = e.readState().codeVoices;
        codePeak = e.readState().channels[engine::kCodeDeckChannel].peakL;
        beatPeak = e.readState().channels[engine::kBeatMakerChannel].peakL;
        beatVoices = e.readState().beatVoices;
        e.send(engine::SetParam{engine::channelParam(engine::kCodeDeckChannel, engine::ChannelParam::Cue), 1.0f});
        rig.render(2);
        cueOn = e.readState().channels[engine::kCodeDeckChannel].cue;
        // hush: the queue emptied and the voices released (the kick fades by itself well within a second).
        rig.producer->submit("hush()");
        rig.render(2);
        e.send(engine::CodeDeckStop{});
        rig.render(static_cast<int>(1.0 * kRate / kBlock));
        voicesAfterStop = e.readState().codeVoices;
    });
    CHECK(voicesPlaying > 0);
    CHECK(codePeak > 0.01f);
    CHECK(beatPeak == 0.0f);
    CHECK(beatVoices == 0);
    CHECK(cueOn);
    CHECK(voicesAfterStop == 0);
    // The FX units can sit on the code deck.
    CHECK(static_cast<int>(engine::FxTarget::CodeDeck) < engine::kFxTargetCount);
    CHECK(engine::kCodeDeckChannel == engine::kMaxChannels - 1);
}
