// P5-17: the code deck render. Done when: the render matches real-time output for a deterministic pattern.

#include "app/CodeDeckProducer.h"
#include "app/CodeRender.h"
#include "app/CodeSoundBank.h"
#include "codebeat/BigStack.h"
#include "engine/AudioEngine.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <vector>

using namespace devildj;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

struct TempFolder
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("devildj-render", "");
    TempFolder() { dir.createDirectory(); }
    ~TempFolder() { dir.deleteRecursively(); }
};

// Real-time playback of `code` as the app runs it: an engine at the tempo, its code deck producer ticked every
// 5 blocks (26.7 ms) on the audio's clock, the sounds installed as they are named. Returns the master output.
std::vector<float> playBack(const std::string& code, double bpm, int samples, const juce::File& folder)
{
    engine::AudioEngine e;
    e.prepare(kRate, kBlock);
    e.send(engine::SetInternalTempo{bpm});
    e.send(engine::SetClockSource{engine::MasterClock::Mode::Internal, engine::MasterClock::kInternal});
    double seconds = 0.0;
    app::CodeDeckProducer producer([&](const engine::ScheduledEvent& ev) { return e.codeDeckEvents().tryPush(ev); }, false);
    producer.now = [&] { return seconds; };
    producer.setSoundBank(std::make_shared<app::CodeSoundBank>(folder));
    producer.submit(code, true);
    const auto tick = [&]
    {
        const auto& s = e.readState();
        producer.setClock({s.sampleClock, s.masterBeat, s.masterBpm, s.sampleRate, seconds});
        producer.tick();
        for (const auto& [index, sound] : producer.takeNewSounds())
        {
            if (sound.kind == app::CodeSound::Kind::Drum)
                e.send(engine::SetDrumSound{index, sound.drum, engine::SourceId::CodeDeck});
            else if (sound.kind == app::CodeSound::Kind::Synth)
                e.send(engine::SetSynthSound{index, sound.synth, engine::SourceId::CodeDeck});
        }
    };
    std::vector<float> out;
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[] = {l.data(), r.data()};
    e.process(outs, 2, kBlock);
    out.insert(out.end(), l.begin(), l.end());
    seconds += kBlock / kRate;
    tick();
    for (int blockIndex = 1; static_cast<int>(out.size()) < samples; ++blockIndex)
    {
        if (blockIndex % 5 == 0)
            tick();
        e.process(outs, 2, kBlock);
        out.insert(out.end(), l.begin(), l.end());
        seconds += kBlock / kRate;
    }
    return out;
}

double rms(const juce::AudioBuffer<float>& b, int from, int to)
{
    double sum = 0.0;
    for (int i = from; i < to; ++i)
        sum += static_cast<double>(b.getSample(0, i)) * b.getSample(0, i);
    return std::sqrt(sum / std::max(1, to - from));
}
} // namespace

TEST_CASE("Code render: matches real-time playback of the same code, sample for sample", "[app][codebeat][render]")
{
    const std::string code = R"(stack(
  s("bd*4, ~ cp, hh*8").gain("1 0.8"),
  note("<c3 e3> g3").s("saw").lpf(1200).room(0.2),
  n("0 3 5").scale("A:minor").s("subbass").delay(0.3)
))";
    TempFolder t;
    app::CodeRenderResult render;
    std::vector<float> live;
    testing::onBigStack([&]
    {
        app::CodeRenderOptions o;
        o.cycles = 4;
        o.bpm = 120.0;
        o.sampleRate = kRate;
        o.blockSize = kBlock;
        o.tailSeconds = 1.0;
        o.samplesFolder = t.dir;
        render = app::renderCode(code, o);
        // Playback long enough to cover the render: its first cycle line (2 s) + 4 cycles + the tail.
        live = playBack(code, 120.0, static_cast<int>(2.0 * kRate) + render.audio->numSamples() + kBlock, t.dir);
    });
    INFO(render.error);
    REQUIRE(render.audio != nullptr);
    // 4 cycles at 120 BPM, 4 beats each: 8 s.
    CHECK(render.loopSamples == static_cast<int>(8.0 * kRate));
    CHECK(render.audio->numSamples() == static_cast<int>(9.0 * kRate));
    CHECK(render.warnings.empty());
    // The render starts at the first cycle line after the start: 2 s into playback.
    const int offset = static_cast<int>(2.0 * kRate);
    REQUIRE(static_cast<int>(live.size()) >= offset + render.loopSamples);
    int differing = 0;
    double worst = 0.0;
    for (int i = 0; i < render.loopSamples; ++i)
    {
        const double d = std::abs(static_cast<double>(render.audio->audio.getSample(0, i)) - live[static_cast<std::size_t>(offset + i)]);
        worst = std::max(worst, d);
        differing += d > 0.0 ? 1 : 0;
    }
    WARN("render vs playback over " << render.loopSamples << " samples: " << differing << " differ, worst " << worst);
    CHECK(differing == 0);
    // And it is not silence.
    CHECK(rms(render.audio->audio, 0, render.loopSamples) > 0.01);
}

TEST_CASE("Code render: the length follows cycles, tempo and setcpb; the tail; nothing after the end", "[app][codebeat][render]")
{
    TempFolder t;
    app::CodeRenderResult a, b, c;
    testing::onBigStack([&]
    {
        app::CodeRenderOptions o;
        o.samplesFolder = t.dir;
        o.sampleRate = kRate;
        o.cycles = 2;
        o.bpm = 128.0;
        o.tailSeconds = 0.0;
        a = app::renderCode(R"(s("bd*4"))", o);
        o.cycles = 3;
        b = app::renderCode("setcpb(3);\ns(\"bd hh hh\")", o);
        o.tailSeconds = 2.0;
        c = app::renderCode(R"(s("bd").release(0.1))", o);
    });
    REQUIRE(a.audio != nullptr);
    CHECK(a.loopSamples == static_cast<int>(std::llround(8.0 * 60.0 * kRate / 128.0))); // 2 cycles x 4 beats
    CHECK(a.audio->numSamples() == a.loopSamples); // no tail: a seamless loop
    REQUIRE(b.audio != nullptr);
    CHECK(b.beatsPerCycle == 3.0);
    CHECK(b.loopSamples == static_cast<int>(std::llround(9.0 * 60.0 * kRate / 128.0))); // 3 cycles x 3 beats
    REQUIRE(c.audio != nullptr);
    // A kick at the start of each of the 3 cycles; the 2 s tail has its decay and then silence - no kick after the end.
    const int tailStart = c.loopSamples;
    CHECK(rms(c.audio->audio, tailStart + static_cast<int>(0.6 * kRate), c.audio->numSamples()) < 1e-4);
}

TEST_CASE("Code render: sample folders are decoded for the render; errors and warnings are reported", "[app][codebeat][render]")
{
    TempFolder t;
    {
        // A 0.2 s click into Samples\mine.
        const auto file = t.dir.getChildFile("mine").getChildFile("click.wav");
        file.getParentDirectory().createDirectory();
        juce::AudioBuffer<float> audio(1, 9600);
        for (int i = 0; i < 9600; ++i)
            audio.setSample(0, i, static_cast<float>(0.5 * std::sin(i * 0.05)));
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(file);
        auto writer = wav.createWriterFor(out, juce::AudioFormatWriterOptions{}.withSampleRate(kRate).withNumChannels(1).withBitsPerSample(16));
        REQUIRE(writer != nullptr);
        writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
    }
    app::CodeRenderResult sample, bad, unknown;
    testing::onBigStack([&]
    {
        app::CodeRenderOptions o;
        o.samplesFolder = t.dir;
        o.cycles = 1;
        o.tailSeconds = 0.0;
        sample = app::renderCode(R"(s("mine*4"))", o);
        bad = app::renderCode(R"(s("bd [sd"))", o);
        unknown = app::renderCode(R"(s("bdd*4"))", o);
    });
    REQUIRE(sample.audio != nullptr);
    CHECK(rms(sample.audio->audio, 0, sample.loopSamples) > 0.01);
    CHECK(sample.error.empty());
    CHECK(bad.audio == nullptr);
    CHECK(bad.error.find("missing \"]\"") != std::string::npos);
    REQUIRE(unknown.audio != nullptr);
    CHECK(unknown.warnings == std::vector<std::string>{"unknown sound \"bdd\" - did you mean \"bd\"?"});
}
