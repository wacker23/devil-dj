// P3-13: sampler deck. Done when: 8 simultaneous looped slots, synced, no xruns.

#include "engine/AudioEngine.h"
#include "engine/Sampler.h"
#include "engine/TrackBuffer.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

using namespace devildj;
using engine::Sampler;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

std::unique_ptr<engine::TrackBuffer> sample(double seconds, double hz = 0.0, float dc = 0.0f)
{
    auto t = std::make_unique<engine::TrackBuffer>();
    t->sampleRate = kRate;
    const int n = static_cast<int>(seconds * kRate);
    t->audio.setSize(2, n);
    for (int i = 0; i < n; ++i)
    {
        const auto v = hz > 0.0 ? static_cast<float>(0.25 * std::sin(2.0 * 3.141592653589793 * hz * i / kRate)) : dc;
        t->audio.setSample(0, i, v);
        t->audio.setSample(1, i, v);
    }
    return t;
}

struct Out
{
    std::vector<float> l, r;
    explicit Out(int n) : l(static_cast<std::size_t>(n)), r(static_cast<std::size_t>(n)) {}
};

Out render(Sampler& s, int blocks, double bpm = 120.0)
{
    Out o(blocks * kBlock);
    for (int b = 0; b < blocks; ++b)
        s.render(o.l.data() + b * kBlock, o.r.data() + b * kBlock, kBlock, bpm);
    return o;
}
} // namespace

TEST_CASE("Sampler: one-shot, loop, gate; a quantized start; stealing; a new sample stops the old", "[engine][sampler]")
{
    Sampler s;
    s.prepare(kRate, kBlock);
    auto dc = sample(0.1, 0.0, 0.5f); // 4800 samples of 0.5
    s.load(0, dc.get(), 7, 0.0);
    SECTION("one-shot: the sample once, to its end")
    {
        s.press(0, 0);
        const auto o = render(s, 30);
        CHECK(o.l[0] == 0.0f);         // the 2 ms fade in
        CHECK(o.l[200] == 0.5f);
        CHECK(o.l[4799] == 0.5f);
        CHECK(o.l[4800] == 0.0f);      // its end
        CHECK_FALSE(s.isPlaying(0));
    }
    SECTION("loop: round and round; a press stops it (fading)")
    {
        s.setMode(0, Sampler::Mode::Loop);
        s.press(0, 0);
        const auto o = render(s, 60);
        CHECK(o.l[4800 * 2 + 10] == 0.5f);
        CHECK(s.isPlaying(0));
        s.press(0, 0);
        render(s, 1);
        CHECK_FALSE(s.isPlaying(0));
    }
    SECTION("gate: while held")
    {
        s.setMode(0, Sampler::Mode::Gate);
        s.press(0, 0);
        render(s, 5);
        CHECK(s.isPlaying(0));
        s.releasePad(0);
        const auto o = render(s, 2);
        CHECK_FALSE(s.isPlaying(0));
        CHECK(o.l[kBlock + 10] == 0.0f);
    }
    SECTION("a quantized start: silent until its sample")
    {
        s.press(0, 1000);
        const auto o = render(s, 10);
        CHECK(o.l[999] == 0.0f);
        CHECK(o.l[1000 + 200] == 0.5f);
    }
    SECTION("gain")
    {
        s.setGain(0, 0.5f);
        s.press(0, 0);
        CHECK(render(s, 4).l[500] == 0.25f);
    }
    SECTION("nine at once: the oldest voice is stolen; a new sample in a slot stops it")
    {
        std::vector<std::unique_ptr<engine::TrackBuffer>> many;
        for (int i = 1; i <= 8; ++i)
        {
            many.push_back(sample(1.0, 0.0, 0.1f));
            s.load(i, many.back().get(), static_cast<engine::BufferId>(10 + i), 0.0);
        }
        for (int i = 0; i <= 8; ++i)
        {
            s.press(i, 0);
            render(s, 1);
        }
        CHECK_FALSE((s.isPlaying(0) && s.isPlaying(1))); // slot 0 had the oldest voice
        int playing = 0;
        for (int i = 0; i <= 8; ++i)
            playing += s.isPlaying(i) ? 1 : 0;
        CHECK(playing == 8);
        CHECK(s.load(5, nullptr, 0, 0.0) == 15); // the old id back for releasing
        CHECK_FALSE(s.isPlaying(5));
    }
}

TEST_CASE("Sampler: 8 looped slots synced to the master tempo stay on its beat, within the time budget",
          "[engine][sampler]")
{
    Sampler s;
    s.prepare(kRate, kBlock);
    // Loops at 120 BPM: 1, 2 and 4 beats long, eight of them, all synced to 126 BPM (stretched by 1.05).
    std::vector<std::unique_ptr<engine::TrackBuffer>> loops;
    const double beat = 0.5; // seconds at 120 BPM
    for (int i = 0; i < 8; ++i)
    {
        const double beats = i % 3 == 0 ? 1.0 : i % 3 == 1 ? 2.0 : 4.0;
        loops.push_back(sample(beats * beat, 110.0 * (i + 1)));
        s.load(i, loops.back().get(), static_cast<engine::BufferId>(i + 1), 120.0);
        s.setMode(i, Sampler::Mode::Loop);
        s.setSync(i, true);
        s.setGain(i, 0.25f);
    }
    for (int i = 0; i < 8; ++i)
        s.press(i, 0);
    constexpr double master = 126.0;
    const int blocks = static_cast<int>(20.0 * kRate / kBlock); // 20 s
    Out o(kBlock);
    double peak = 0.0;
    bool finite = true;
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b)
    {
        std::fill(o.l.begin(), o.l.end(), 0.0f);
        std::fill(o.r.begin(), o.r.end(), 0.0f);
        s.render(o.l.data(), o.r.data(), kBlock, master);
        for (const float v : o.l)
        {
            finite = finite && std::isfinite(v);
            peak = std::max(peak, static_cast<double>(std::abs(v)));
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double load = seconds / 20.0;
    WARN("8 synced loops: " << 100.0 * load << " % of real time");
    CHECK(finite);
    CHECK(peak > 0.05);
    // On the beat: each loop's place in it, in beats, = the master's beats since the start (mod its length).
    const double masterBeats = blocks * kBlock * master / (60.0 * kRate);
    for (int i = 0; i < 8; ++i)
    {
        INFO("loop " << i);
        REQUIRE(s.isPlaying(i));
        const double beats = i % 3 == 0 ? 1.0 : i % 3 == 1 ? 2.0 : 4.0;
        const double expected = std::fmod(masterBeats, beats) / beats;
        const double d = std::abs(s.progress(i) - expected);
        CHECK(std::min(d, 1.0 - d) < 1e-6); // round the loop (at its end = at its start)
    }
#ifdef NDEBUG
    CHECK(load < 0.5); // no xruns: well inside the time the blocks last (Release)
#endif
}

TEST_CASE("Sampler in the engine: its own channel (fader, cue, crossfader), quantized to the clock", "[engine][sampler]")
{
    engine::AudioEngine e;
    e.prepare(kRate, kBlock);
    auto dc = sample(1.0, 0.0, 0.5f);
    e.send(engine::SamplerLoad{3, dc.get(), 42, 0.0f});
    float l[kBlock], r[kBlock];
    float* outs[] = {l, r};
    e.process(outs, 2, kBlock);
    CHECK(e.readState().sampler[3].loaded);
    CHECK(e.readState().sampler[3].mode == 0);
    e.send(engine::SamplerPad{3, false});
    e.process(outs, 2, kBlock);
    e.process(outs, 2, kBlock);
    CHECK(e.readState().sampler[3].playing);
    CHECK(l[100] > 0.2f); // through the sampler channel (centre crossfader: x 0.71)
    CHECK(e.readState().channels[engine::kSamplerChannel].peakL > 0.2f);
    // Its fader down: silent.
    e.send(engine::SetParam{engine::channelParam(engine::kSamplerChannel, engine::ChannelParam::Fader), 0.0f});
    for (int b = 0; b < 10; ++b)
        e.process(outs, 2, kBlock);
    CHECK(std::abs(l[100]) < 1e-6f);
    // Settings.
    e.send(engine::SamplerSet{3, engine::SamplerOp::Mode, 1.0f});
    e.send(engine::SamplerSet{3, engine::SamplerOp::Gain, 0.5f});
    e.send(engine::SamplerSet{3, engine::SamplerOp::Sync, 1.0f});
    e.process(outs, 2, kBlock);
    CHECK(e.readState().sampler[3].mode == 1);
    CHECK(e.readState().sampler[3].gain == 0.5f);
    CHECK(e.readState().sampler[3].sync);
    e.send(engine::SamplerSet{0, engine::SamplerOp::StopAll, 0.0f});
    e.process(outs, 2, kBlock);
    e.process(outs, 2, kBlock);
    CHECK_FALSE(e.readState().sampler[3].playing);
    // Quantized: with quantize on, the start waits for the master clock's next beat.
    e.send(engine::SetParam{engine::channelParam(engine::kSamplerChannel, engine::ChannelParam::Fader), 1.0f});
    e.send(engine::SetQuantize{true, 1.0f});
    e.process(outs, 2, kBlock);
    const double beatNow = e.readState().masterBeat;
    e.send(engine::SamplerPad{3, false});
    int waited = 0;
    while (!(std::abs(l[kBlock - 1]) > 0.01f) && waited < 200)
    {
        e.process(outs, 2, kBlock);
        ++waited;
    }
    const double beatThen = e.readState().masterBeat;
    CHECK(std::floor(beatThen) > std::floor(beatNow)); // it began after the next beat came
    CHECK(waited * kBlock < 0.6 * kRate);              // within one beat at 120
    // A new sample in the slot gives the old buffer back for releasing.
    auto other = sample(0.5, 0.0, 0.1f);
    e.send(engine::SamplerLoad{3, other.get(), 43, 0.0f});
    e.process(outs, 2, kBlock);
    engine::BufferId released = 0;
    CHECK(e.popReleasedBuffer(released));
    CHECK(released == 42);
}

#include "ui/SamplerPanel.h"

TEST_CASE("Sampler panel: slots, mode, sync, banks, its channel's cue and crossfader side", "[ui][sampler]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    engine::AudioEngine e;
    e.prepare(kRate, kBlock);
    float l[kBlock], r[kBlock];
    float* outs[] = {l, r};
    auto loop = sample(2.0, 220.0);
    e.send(engine::SamplerLoad{9, loop.get(), 5, 120.0f}); // bank 2, slot 2
    e.process(outs, 2, kBlock);
    ui::SamplerPanel panel(e);
    panel.setBounds(0, 0, 1100, ui::SamplerPanel::kPreferredHeight);
    int bankSeen = -1;
    panel.onBank = [&](int b) { bankSeen = b; };
    panel.refresh();
    CHECK_FALSE(panel.tile(1).play.isEnabled()); // bank 1: empty
    panel.bankButton(1).onClick();
    CHECK(bankSeen == 1);
    panel.refresh();
    REQUIRE(panel.tile(1).play.isEnabled());
    CHECK(panel.tile(1).sync.isEnabled()); // it has a BPM
    panel.tile(1).mode.onClick();          // one-shot -> loop
    panel.tile(1).sync.onClick();
    e.process(outs, 2, kBlock);
    CHECK(e.readState().sampler[9].mode == 1);
    CHECK(e.readState().sampler[9].sync);
    panel.tile(1).play.setState(juce::Button::buttonDown);
    panel.tile(1).play.setState(juce::Button::buttonNormal);
    e.process(outs, 2, kBlock);
    CHECK(e.readState().sampler[9].playing);
    panel.cueButton().onClick();
    panel.assignButton().onClick();
    e.process(outs, 2, kBlock);
    CHECK(e.readState().channels[engine::kSamplerChannel].cue);
    panel.refresh();
    CHECK(panel.assignButton().getButtonText() == "B"); // THRU -> B
    juce::Image image(juce::Image::ARGB, 1100, ui::SamplerPanel::kPreferredHeight, true);
    juce::Graphics g(image);
    panel.paintEntireComponent(g, false);
}
