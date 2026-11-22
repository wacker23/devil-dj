// P3-15: sequencer. Done when: recorded sequence replays identically after restart.

#include "SequenceStore.h"
#include "engine/AudioEngine.h"
#include "engine/Sampler.h"
#include "engine/Sequencer.h"
#include "engine/TrackBuffer.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace devildj;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

std::unique_ptr<engine::TrackBuffer> click(float level)
{
    auto t = std::make_unique<engine::TrackBuffer>();
    t->sampleRate = kRate;
    t->audio.setSize(2, 2400);
    for (int i = 0; i < 2400; ++i)
    {
        t->audio.setSample(0, i, level);
        t->audio.setSample(1, i, level);
    }
    return t;
}

// Where each sound starts (the first non-zero sample after silence) in a run of the sampler with a sequencer at
// 120 BPM from master beat 0.
std::vector<int> onsets(engine::Sequencer& seq, engine::Sampler& sampler, int blocks)
{
    std::vector<int> found;
    std::vector<float> l(kBlock), r(kBlock);
    const double perBeat = 60.0 / 120.0 * kRate;
    bool sounding = false;
    for (int b = 0; b < blocks; ++b)
    {
        const double start = b * kBlock / perBeat, end = (b + 1) * kBlock / perBeat;
        seq.process(sampler, start, end, perBeat, kBlock);
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        sampler.render(l.data(), r.data(), kBlock, 120.0);
        for (int i = 0; i < kBlock; ++i)
        {
            const bool now = l[static_cast<std::size_t>(i)] != 0.0f;
            if (now && !sounding)
                found.push_back(b * kBlock + i);
            sounding = now;
        }
    }
    return found;
}
} // namespace

TEST_CASE("Sequencer: events sound at their exact sample, every pass; 1/16 quantize; overdub, mute, erase, bars",
          "[engine][sequencer]")
{
    engine::Sampler sampler;
    sampler.prepare(kRate, kBlock);
    auto a = click(0.5f), b = click(0.25f);
    sampler.load(0, a.get(), 1, 0.0);
    sampler.load(5, b.get(), 2, 0.0);
    engine::Sequencer seq;
    seq.reset();
    seq.clear(1); // 4 beats = 96000 samples at 120
    REQUIRE(seq.add({0.0f, 0, 0}));
    REQUIRE(seq.add({1.5f, 0, 0}));
    REQUIRE(seq.add({2.75f, 5, 0}));
    seq.setPlaying(true);
    const auto at = onsets(seq, sampler, 2 * 96000 / kBlock); // exactly two passes
    // Each a sample after its beat (the 2 ms fade-in starts at 0), two passes.
    const std::vector<int> expected{1, 36001, 66001, 96001, 132001, 162001};
    CHECK(at == expected);

    // Recording: a press at beat 0.97 goes in at 1.0 (the nearest 1/16 = quarter beat); the same spot twice: once.
    engine::Sequencer rec;
    rec.reset();
    rec.clear(2);
    rec.setRecording(true);
    CHECK(rec.playing());
    rec.record(5, false, 8.0 + 0.97);
    rec.record(5, false, 8.0 + 1.02);
    REQUIRE(rec.sequence().count == 1);
    CHECK(rec.sequence().events[0].beat == 1.0f);
    rec.record(0, false, 7.99); // 7.99 -> beat 8 of an 8-beat loop = 0, the next pass
    CHECK(rec.sequence().events[1].beat == 0.0f);
    // Mute, erase a slot, shorter loop (events past it go).
    rec.record(0, false, 6.5);
    rec.setMuted(5, true);
    CHECK(rec.mutedSlots() == 1u << 5);
    rec.eraseSlot(5);
    CHECK(rec.sequence().count == 2);
    rec.setBars(1);
    CHECK(rec.sequence().count == 1); // 6.5 doesn't fit 4 beats
    rec.erase();
    CHECK(rec.sequence().count == 0);
}

TEST_CASE("Sequencer: a press recorded live isn't doubled in the pass it was played; it sounds from the next",
          "[engine][sequencer]")
{
    engine::Sampler sampler;
    sampler.prepare(kRate, kBlock);
    auto a = click(0.5f);
    sampler.load(0, a.get(), 1, 0.0);
    engine::Sequencer seq;
    seq.reset();
    seq.clear(1);
    seq.setRecording(true);
    // A press at master beat 1.98: heard live now (as the engine plays it), recorded at 2.0 - just ahead.
    seq.record(0, false, 1.98);
    const auto at = onsets(seq, sampler, 2 * 96000 / kBlock + 2);
    // Not at 2.0 of this pass (48000) - only at 2.0 of the next (96000 + 48000).
    REQUIRE(at.size() == 1);
    CHECK(at[0] == 96000 + 48000 + 1);
}

TEST_CASE("Sequencer: a recorded sequence, saved and loaded in a new engine (a restart), replays identically",
          "[engine][sequencer][app]")
{
    auto a = click(0.5f), b = click(0.3f);
    float l[kBlock], r[kBlock];
    float* outs[] = {l, r};
    const auto setup = [&](engine::AudioEngine& e)
    {
        e.prepare(kRate, kBlock);
        e.send(engine::SetInternalTempo{120.0});
        e.send(engine::SamplerLoad{0, a.get(), 0, 0.0f});
        e.send(engine::SamplerLoad{1, b.get(), 0, 0.0f});
        e.send(engine::SetQuantize{true, 0.25f});
        e.process(outs, 2, kBlock);
    };
    // Engine 1: record a 1-bar sequence by pressing pads during the first pass.
    engine::AudioEngine first;
    setup(first);
    first.send(engine::SequencerCommand{engine::SequencerOp::Clear, 0, 1.0f});
    first.send(engine::SequencerCommand{engine::SequencerOp::Record, 0, 1.0f});
    int block = 1;
    const std::vector<std::pair<int, int>> presses{{30, 0}, {110, 1}, {200, 0}, {290, 1}}; // (block, slot)
    std::vector<float> firstPass2;
    const int passBlocks = 96000 / kBlock; // 375
    for (; block < 3 * passBlocks; ++block)
    {
        for (const auto& [at, slot] : presses)
            if (block == at)
                first.send(engine::SamplerPad{static_cast<std::uint8_t>(slot), false});
        if (block == passBlocks + 10)
            first.send(engine::SequencerCommand{engine::SequencerOp::Record, 0, 0.0f}); // recording off, plays on
        first.process(outs, 2, kBlock);
        if (block >= 2 * passBlocks && block < 3 * passBlocks)
            firstPass2.insert(firstPass2.end(), l, l + kBlock);
    }
    REQUIRE(first.readState().sequencer.sequence.count == 4);
    CHECK(first.readState().sequencer.playing);
    CHECK_FALSE(first.readState().sequencer.recording);

    // Saved (the settings' JSON) and read back after a "restart".
    app::SequenceStore store;
    store[0][3] = app::fromEngine(first.readState().sequencer.sequence);
    const auto json = app::toJson(store);
    const auto back = app::sequencesFromJson(json);
    REQUIRE(back[0][3].events.size() == 4);
    CHECK(back[0][3].bars == 1);

    // Engine 2: a fresh one; load the sequence and play: its third pass sounds exactly like the first engine's.
    engine::AudioEngine second;
    setup(second);
    for (const auto& c : app::loadCommands(back[0][3]))
        second.send(c);
    second.send(engine::SequencerCommand{engine::SequencerOp::Play, 0, 1.0f});
    std::vector<float> secondPass2;
    for (int bl = 1; bl < 3 * passBlocks; ++bl)
    {
        second.process(outs, 2, kBlock);
        if (bl >= 2 * passBlocks && bl < 3 * passBlocks)
            secondPass2.insert(secondPass2.end(), l, l + kBlock);
    }
    REQUIRE(firstPass2.size() == secondPass2.size());
    CHECK(std::memcmp(firstPass2.data(), secondPass2.data(), sizeof(float) * firstPass2.size()) == 0);
    double energy = 0.0;
    for (const float v : secondPass2)
        energy += static_cast<double>(v) * v;
    CHECK(energy > 1.0); // (it isn't two silences)
    // Rubbish in the settings: empty sequences.
    CHECK(app::sequencesFromJson("{]")[0][0].empty());
}

#include "ui/DeckView.h"
#include "ui/SamplerPanel.h"

TEST_CASE("Sequencer controls: REC, play, bars, erase, metronome in the sampler panel; Sequence Call pads",
          "[ui][sequencer]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    engine::AudioEngine e;
    e.prepare(kRate, kBlock);
    float l[kBlock], r[kBlock];
    float* outs[] = {l, r};
    ui::SamplerPanel panel(e);
    panel.setBounds(0, 0, 1200, ui::SamplerPanel::kPreferredHeight);
    panel.recButton().onClick();
    panel.barsButton().onClick();
    panel.metroButton().onClick();
    e.process(outs, 2, kBlock);
    CHECK(e.readState().sequencer.recording);
    CHECK(e.readState().sequencer.playing);
    CHECK(e.readState().sequencer.sequence.bars == 2);
    CHECK(e.readState().sequencer.metronome);
    panel.refresh();
    CHECK(panel.barsButton().getButtonText() == "2 BARS");
    CHECK(panel.recButton().getToggleState());
    panel.recButton().onClick(); // stop recording: plays on
    panel.eraseButton().onClick();
    e.process(outs, 2, kBlock);
    CHECK_FALSE(e.readState().sequencer.recording);
    CHECK(e.readState().sequencer.playing);
    panel.seqPlayButton().onClick();
    e.process(outs, 2, kBlock);
    CHECK_FALSE(e.readState().sequencer.playing);
    int chosen = -1;
    panel.onSequence = [&](int i) { chosen = i; };
    panel.onSequence(4);
    CHECK(chosen == 4);
    panel.setSequenceIndex(4);
    CHECK(panel.seqButton().getButtonText() == "SEQ 5");

    ui::DeckView view(e, 0, [](engine::BufferId) { return nullptr; });
    view.setBounds(0, 0, 700, 330);
    view.setPadMode(ui::PadMode::SequenceCall);
    std::array<bool, 8> filled{};
    filled[2] = true;
    view.setSequenceLooks(filled, 2);
    e.send(engine::SequencerCommand{engine::SequencerOp::Play, 0, 1.0f});
    e.process(outs, 2, kBlock);
    view.refresh();
    CHECK(view.padButton(2).getButtonText() == "SEQ 3");
    CHECK(view.padButton(0).getButtonText() == "-");
    CHECK(view.padButton(2).findColour(juce::TextButton::buttonColourId) == juce::Colour(0xff3cc4b4)); // playing
    CHECK(view.padModeButton().getButtonText() == "SEQ CALL");
}
