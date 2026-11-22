// P3-08: FX framework. Done when: unit tests for tail handling and bypass (bit-exact when off).

#include "dsp/fx/FxSlot.h"
#include "dsp/fx/FxUnit.h"
#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace devildj;
using dsp::fx::FxSlot;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

// Test effects.
class Echo final : public dsp::fx::Effect // 100 ms, feedback 0.5: has a tail
{
public:
    const char* name() const noexcept override { return "Echo (test)"; }
    void prepare(double sampleRate, int) override
    {
        line.assign(static_cast<std::size_t>(0.1 * sampleRate), 0.0f);
        reset();
    }
    void reset() noexcept override
    {
        std::fill(line.begin(), line.end(), 0.0f);
        at = 0;
    }
    void process(float* l, float* r, int n, const dsp::fx::BeatContext&) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            const float out = line[at];
            line[at] = 0.5f * (l[i] + r[i]) + 0.5f * out;
            at = (at + 1) % line.size();
            l[i] = r[i] = out;
        }
    }
    bool hasTail() const noexcept override { return true; }

private:
    std::vector<float> line;
    std::size_t at = 0;
};

class Invert final : public dsp::fx::Effect // no tail
{
public:
    const char* name() const noexcept override { return "Invert (test)"; }
    void prepare(double, int) override {}
    void reset() noexcept override {}
    void process(float* l, float* r, int n, const dsp::fx::BeatContext&) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            l[i] = -l[i];
            r[i] = -r[i];
        }
    }
};

class Drone final : public dsp::fx::Effect // a "tail" that never dies away
{
public:
    const char* name() const noexcept override { return "Drone (test)"; }
    void prepare(double, int) override {}
    void reset() noexcept override { phase = 0.0; }
    void process(float* l, float* r, int n, const dsp::fx::BeatContext&) noexcept override
    {
        for (int i = 0; i < n; ++i, phase += 0.05)
            l[i] = r[i] = static_cast<float>(0.1 * std::sin(phase));
    }
    bool hasTail() const noexcept override { return true; }

private:
    double phase = 0.0;
};

struct Signal
{
    std::vector<float> l, r;
    explicit Signal(int n, double hz = 220.0, double level = 0.5) : l(static_cast<std::size_t>(n)), r(l.size())
    {
        for (int i = 0; i < n; ++i)
        {
            l[static_cast<std::size_t>(i)] = static_cast<float>(level * std::sin(2.0 * 3.141592653589793 * hz * i / kRate));
            r[static_cast<std::size_t>(i)] = static_cast<float>(level * std::cos(2.0 * 3.141592653589793 * hz * i / kRate));
        }
    }
};

// Runs `blocks` blocks of a signal through the slot; returns the output.
Signal run(FxSlot& slot, int blocks, double level = 0.5)
{
    Signal s(blocks * kBlock, 220.0, level);
    for (int b = 0; b < blocks; ++b)
        slot.process(s.l.data() + b * kBlock, s.r.data() + b * kBlock, kBlock, {});
    return s;
}

bool bitExact(const Signal& a, const Signal& b)
{
    return a.l.size() == b.l.size() && std::memcmp(a.l.data(), b.l.data(), a.l.size() * sizeof(float)) == 0 &&
           std::memcmp(a.r.data(), b.r.data(), a.r.size() * sizeof(float)) == 0;
}
} // namespace

TEST_CASE("FX: off is bit-exact (empty, never on, and after it has been on)", "[dsp][fx]")
{
    Echo echo;
    echo.prepare(kRate, kBlock);
    FxSlot slot;
    slot.prepare(kRate, kBlock);
    const Signal dry(40 * kBlock);
    CHECK(bitExact(run(slot, 40), dry)); // empty slot
    slot.setEffect(&echo);
    slot.setWet(0.7f);
    CHECK(bitExact(run(slot, 40), dry)); // an effect, off
    slot.setOn(true);
    CHECK_FALSE(bitExact(run(slot, 40), dry));
    slot.setOn(false);
    for (int i = 0; i < 2000 && slot.isActive(); ++i)
        run(slot, 1, 0.0);
    REQUIRE(slot.stage() == FxSlot::Stage::Off);
    CHECK(bitExact(run(slot, 40), dry)); // off again: untouched

    // A unit with every slot off; and in single mode slots 1-2 can't be switched on.
    dsp::fx::FxUnit unit;
    unit.prepare(kRate, kBlock);
    Invert inv;
    unit.slot(1).setEffect(&inv);
    Signal s(10 * kBlock);
    for (int b = 0; b < 10; ++b)
        unit.process(s.l.data() + b * kBlock, s.r.data() + b * kBlock, kBlock, {});
    CHECK(bitExact(s, Signal(10 * kBlock)));
    unit.slot(1).setOn(true);
    CHECK(unit.isActive());
    unit.setMode(dsp::fx::FxUnit::Mode::Single);
    CHECK_FALSE(unit.slot(1).isOn());
    CHECK_FALSE(unit.slotUsable(1));
    CHECK(unit.slotUsable(0));
}

TEST_CASE("FX: an effect without a tail fades in and out in 20 ms, without a step", "[dsp][fx]")
{
    Invert inv;
    FxSlot slot;
    slot.prepare(kRate, kBlock);
    slot.setEffect(&inv);
    slot.setWet(1.0f);
    slot.setOn(true);
    auto out = run(slot, 20);
    const Signal dry(20 * kBlock);
    // After the 20 ms (960 samples): fully wet = inverted.
    for (std::size_t i = 1000; i < out.l.size(); ++i)
        REQUIRE(std::abs(out.l[i] + dry.l[i]) < 1e-6f);
    // On the way: (1 - 2m) x the input, m a ramp - no jump bigger than the sine's own slope.
    double worst = 0.0;
    for (std::size_t i = 1; i < 1000; ++i)
        worst = std::max(worst, static_cast<double>(std::abs(out.l[i] - out.l[i - 1])));
    CHECK(worst < 2.0 * 3.141592653589793 * 220.0 / kRate * 0.5 * 1.1);
    slot.setOn(false);
    run(slot, 3); // 768 samples: still fading
    CHECK(slot.stage() == FxSlot::Stage::FadingOut);
    run(slot, 2);
    CHECK(slot.stage() == FxSlot::Stage::Off); // no tail to wait for
}

TEST_CASE("FX: an effect with a tail rings out after it is switched off, then the slot is off", "[dsp][fx]")
{
    Echo echo;
    echo.prepare(kRate, kBlock);
    FxSlot slot;
    slot.prepare(kRate, kBlock);
    slot.setEffect(&echo);
    slot.setWet(0.5f);
    slot.setOn(true);
    run(slot, 40); // ~0.2 s of tone into the echo
    slot.setOn(false);
    run(slot, 5, 0.0); // the send closes in 20 ms
    CHECK(slot.stage() == FxSlot::Stage::Tail);
    // Silence in: the echoes are still heard for a while...
    const auto heard = run(slot, 40, 0.0);
    double energy = 0.0;
    for (const float v : heard.l)
        energy += v * v;
    CHECK(energy > 1.0);
    // ...and the dry signal is back at full level: a tone in now = tone + echoes of the old one.
    // Switched on again while ringing: the echo goes on (not reset).
    slot.setOn(true);
    const auto again = run(slot, 1, 0.0);
    CHECK(std::abs(again.l[10]) > 1e-4f);
    slot.setOn(false);
    // The echo halves every 100 ms: below -100 dBFS after ~1.7 s; Off within 3 s, then bit-exact.
    int blocks = 0;
    while (slot.isActive() && blocks < 600)
    {
        run(slot, 1, 0.0);
        ++blocks;
    }
    CHECK(slot.stage() == FxSlot::Stage::Off);
    CHECK(blocks * kBlock < 3.0 * kRate);
    CHECK(bitExact(run(slot, 10), Signal(10 * kBlock)));
}

TEST_CASE("FX: a tail that never dies away is cut after 30 s", "[dsp][fx]")
{
    Drone drone;
    FxSlot slot;
    slot.prepare(kRate, 4096);
    slot.setEffect(&drone);
    slot.setOn(true);
    std::vector<float> l(4096), r(4096);
    slot.process(l.data(), r.data(), 4096, {});
    slot.setOn(false);
    int samples = 0;
    while (slot.isActive() && samples < 40 * 48000)
    {
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
        slot.process(l.data(), r.data(), 4096, {});
        samples += 4096;
    }
    CHECK(slot.stage() == FxSlot::Stage::Off);
    CHECK(samples >= 30 * 48000);
    CHECK(samples < 31 * 48000);
}

TEST_CASE("FX in the engine: off = bit-exact on a deck; targets; replaced effects come back", "[engine][fx]")
{
    engine::TrackBuffer track;
    track.sampleRate = kRate;
    track.audio.setSize(2, static_cast<int>(10.0 * kRate));
    for (int i = 0; i < track.audio.getNumSamples(); ++i)
    {
        const auto v = static_cast<float>(0.3 * std::sin(2.0 * 3.141592653589793 * 330.0 * i / kRate));
        track.audio.setSample(0, i, v);
        track.audio.setSample(1, i, v);
    }
    engine::AudioEngine plain, withFx;
    for (auto* e : {&plain, &withFx})
    {
        e->prepare(kRate, kBlock);
        e->send(engine::LoadTrack{0, &track, 1});
        e->send(engine::Play{0});
    }
    REQUIRE(withFx.installEffect(0, 0, std::make_unique<Invert>()));
    withFx.send(engine::FxCommand{0, engine::FxOp::Target, 0, 0, static_cast<float>(engine::FxTarget::Deck0)});
    float pl[kBlock], pr[kBlock], fl[kBlock], fr[kBlock];
    float* p[] = {pl, pr};
    float* f[] = {fl, fr};
    const auto blocks = [&](int n)
    {
        bool same = true;
        double difference = 0.0;
        for (int b = 0; b < n; ++b)
        {
            plain.process(p, 2, kBlock);
            withFx.process(f, 2, kBlock);
            same = same && std::memcmp(pl, fl, sizeof(pl)) == 0 && std::memcmp(pr, fr, sizeof(pr)) == 0;
            for (int i = 0; i < kBlock; ++i)
                difference = std::max(difference, static_cast<double>(std::abs(pl[i] - fl[i])));
        }
        return std::pair{same, difference};
    };
    CHECK(blocks(50).first); // the effect is there, off: the same to the bit
    CHECK(withFx.readState().fx[0].target == engine::FxTarget::Deck0);
    CHECK(withFx.readState().fx[0].slots[0].hasEffect);

    withFx.send(engine::FxCommand{0, engine::FxOp::On, 0, 0, 1.0f});
    blocks(10);
    CHECK(withFx.readState().fx[0].slots[0].stage == 1);
    CHECK(blocks(10).second > 0.1); // on: inverted

    // On a target with nothing to process (the sampler, until P3-13): the deck is untouched again.
    withFx.send(engine::FxCommand{0, engine::FxOp::Target, 0, 0, static_cast<float>(engine::FxTarget::Sampler)});
    blocks(1);
    CHECK(blocks(20).first);
    // On the master: heard again.
    withFx.send(engine::FxCommand{0, engine::FxOp::Target, 0, 0, static_cast<float>(engine::FxTarget::Master)});
    blocks(10);
    CHECK(blocks(10).second > 0.1);

    // Replacing the effect hands the old one back to the message thread.
    CHECK(withFx.ownedEffectCount() == 1);
    REQUIRE(withFx.installEffect(0, 0, std::make_unique<Echo>()));
    CHECK(withFx.ownedEffectCount() == 2);
    blocks(1);
    CHECK(withFx.collectEffects() == 1);
    CHECK(withFx.ownedEffectCount() == 1);
    REQUIRE(withFx.installEffect(0, 0, nullptr)); // empty the slot
    blocks(1);
    CHECK(withFx.collectEffects() == 1);
    CHECK(withFx.ownedEffectCount() == 0);
    CHECK_FALSE(withFx.readState().fx[0].slots[0].hasEffect);
}
