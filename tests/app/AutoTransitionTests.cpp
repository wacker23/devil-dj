// P4-16: the Auto transition. Done when: on generated tracks with known phrases it picks phrase lines (the owner
// rates real ones from DEVILDJ_EVAL_DIR separately).

#include "app/AutoTransition.h"
#include "util/MusicalKey.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace devildj;
using app::TransitionPreset;

namespace
{
constexpr double kRate = 48000.0;

// A track of `bars` bars at `bpm` whose bars [bassFrom, bassTo) have bass, all bars up to `contentBars` have
// mids and highs (louder by `gainDb`), silence after; its first downbeat `offsetBeats` beats after the grid's
// first beat (the grid's first beat at 0.25 s).
struct Synth
{
    analysis::Waveform waveform;
    util::Beatgrid grid;
    int numSamples = 0;
};

Synth make(int bars, int bassFrom, int bassTo, int contentBars, double bpm = 120.0, int offsetBeats = 0, float gainDb = 0.0f)
{
    Synth s;
    const double first = 0.25 * kRate;
    const int barBeat = offsetBeats == 0 ? 1 : 5 - offsetBeats; // the grid's first beat is beat `barBeat` of its bar
    s.grid = util::Beatgrid::constant(first, bpm, kRate, barBeat);
    const double beatSamples = 60.0 * kRate / bpm;
    s.numSamples = static_cast<int>(first + (offsetBeats + 4.0 * bars) * beatSamples + kRate);
    auto& w = s.waveform;
    w.sampleRate = kRate;
    w.samplesPerPoint = static_cast<int>(std::lround(kRate / analysis::Waveform::kPointsPerSecond));
    const int points = s.numSamples / w.samplesPerPoint;
    const float g = std::pow(10.0f, gainDb / 20.0f);
    for (auto& band : w.rms)
        band.assign(static_cast<std::size_t>(points), 0.0f);
    for (auto& band : w.peak)
        band.assign(static_cast<std::size_t>(points), 0.0f);
    for (int p = 0; p < points; ++p)
    {
        const double sample = (p + 0.5) * w.samplesPerPoint;
        const double barPos = ((sample - first) / beatSamples - offsetBeats) / 4.0;
        if (barPos < 0.0)
            continue;
        const int bar = static_cast<int>(barPos);
        if (bar >= contentBars)
            continue;
        w.rms[0][static_cast<std::size_t>(p)] = (bar >= bassFrom && bar < bassTo ? 0.3f : 0.002f) * g;
        w.rms[1][static_cast<std::size_t>(p)] = 0.15f * g;
        w.rms[2][static_cast<std::size_t>(p)] = 0.08f * g;
    }
    return s;
}

app::AutoInput input(const Synth& s, int key, double bpm = 120.0)
{
    return {&s.waveform, s.grid, s.numSamples, key, bpm};
}

bool onPhraseLine(double beat, double firstDownbeat)
{
    const double bars = (beat - firstDownbeat) / 4.0;
    return std::abs(bars - std::round(bars)) < 1e-9 && static_cast<long long>(std::round(bars)) % 4 == 0;
}
} // namespace

TEST_CASE("Auto transition: the intro, the outro and the end found on the phrase lines", "[app][transition][auto]")
{
    const auto out = make(64, 8, 56, 64);
    const auto bars = app::barProfile(out.waveform, out.grid, out.numSamples);
    REQUIRE(bars.size() == 64);
    CHECK(bars[10].low > -15.0f);
    CHECK(bars[60].low < -40.0f);
    const auto shape = app::trackShape(bars, app::firstDownbeatBeat(out.grid));
    CHECK(shape.introEnd == 8);
    CHECK(shape.outroStart == 56);
    CHECK(shape.contentEnd == 64);
    // Silence after its end, no outro, no intro.
    const auto plain = make(40, 0, 32, 32);
    const auto s2 = app::trackShape(app::barProfile(plain.waveform, plain.grid, plain.numSamples), 0.0);
    CHECK(s2.contentEnd == 32);
    CHECK(s2.outroStart == 32);
    CHECK(s2.introEnd == 0);
}

TEST_CASE("Auto transition: the new bass lands where the old one leaves, on a phrase line", "[app][transition][auto]")
{
    // The old track: bass bars 8 .. 55, an 8-bar outro; the new one: a 16-bar intro without bass.
    const auto out = make(64, 8, 56, 64);
    const auto in = make(48, 16, 48, 48);
    const auto plan = app::planAutoTransition(input(out, 21), input(in, 21));
    REQUIRE(plan.ok);
    for (const auto& r : plan.reasons)
        WARN(r);
    // From bar 40 (= 56 - 16) to the end (bar 64): 24 bars, the basses swapped at bar 16 of them.
    CHECK(plan.outBeat == app::firstDownbeatBeat(out.grid) + 4.0 * 40);
    CHECK(onPhraseLine(plan.outBeat, app::firstDownbeatBeat(out.grid)));
    CHECK(plan.bars == 24);
    CHECK(plan.swapBar == 16.0f);
    CHECK(plan.inBeat == app::firstDownbeatBeat(in.grid));
    CHECK(plan.preset == TransitionPreset::LongBlend);
    const auto& outLow = plan.curves.lane(false, engine::Lane::EqLow);
    const auto& inLow = plan.curves.lane(true, engine::Lane::EqLow);
    CHECK(engine::evaluate(outLow, 63.9) == 0.0f);
    CHECK(engine::evaluate(outLow, 64.0) <= -100.0f);
    CHECK(engine::evaluate(inLow, 63.9) <= -100.0f);
    CHECK(engine::evaluate(inLow, 64.0) == 0.0f);
    CHECK(plan.curves.beats == 96.0f);
    CHECK(plan.reasons.size() >= 2);
}

TEST_CASE("Auto transition: the preset follows the music", "[app][transition][auto]")
{
    const auto out = make(64, 8, 56, 64);
    // A new track that starts with its bass: in at the outro, a smooth blend.
    {
        const auto in = make(48, 0, 48, 48);
        const auto plan = app::planAutoTransition(input(out, 21), input(in, 21));
        REQUIRE(plan.ok);
        CHECK(plan.preset == TransitionPreset::SmoothBlend);
        CHECK(plan.outBeat == 4.0 * 56);
        CHECK(plan.bars == 8);
    }
    // More energy coming (6 dB louder, an 8-bar intro): it rises out of a filter.
    {
        const auto in = make(48, 8, 48, 48, 120.0, 0, 6.0f);
        CHECK(app::planAutoTransition(input(out, 21), input(in, 21)).preset == TransitionPreset::FilterRise);
    }
    // An 8-bar intro, the same energy: a bass swap.
    {
        const auto in = make(48, 8, 48, 48);
        const auto plan = app::planAutoTransition(input(out, 21), input(in, 21));
        CHECK(plan.preset == TransitionPreset::BassSwap);
        CHECK(plan.outBeat == 4.0 * 48);
    }
    // Keys that clash: short and filtered away.
    {
        const auto in = make(48, 16, 48, 48);
        int clash = 0;
        while (util::harmonicallyCompatible(21, clash))
            ++clash;
        const auto plan = app::planAutoTransition(input(out, 21), input(in, clash));
        CHECK(plan.preset == TransitionPreset::HighPassOut);
        CHECK(plan.bars <= 8);
        CHECK(onPhraseLine(plan.outBeat, 0.0));
    }
    // An old track that ends at full level: out on an echo over its last 4 bars.
    {
        const auto hot = make(64, 0, 64, 64);
        const auto in = make(48, 16, 48, 48);
        const auto plan = app::planAutoTransition(input(hot, 21), input(in, 21));
        CHECK(plan.preset == TransitionPreset::EchoOut);
        CHECK(plan.bars == 4);
        CHECK(plan.outBeat == 4.0 * 60);
    }
    // Tempi 10 % apart: an echo out too.
    {
        const auto in = make(48, 16, 48, 48, 132.0);
        CHECK(app::planAutoTransition(input(out, 21, 120.0), input(in, 21, 132.0)).preset == TransitionPreset::EchoOut);
    }
}

TEST_CASE("Auto transition: a grid whose first beat isn't a downbeat; missing analysis", "[app][transition][auto]")
{
    // The first downbeat two beats after the grid's first beat.
    const auto out = make(64, 8, 56, 64, 120.0, 2);
    const auto in = make(48, 16, 48, 48, 120.0, 2);
    CHECK(app::firstDownbeatBeat(out.grid) == 2.0);
    const auto plan = app::planAutoTransition(input(out, 21), input(in, 21));
    REQUIRE(plan.ok);
    CHECK(plan.outBeat == 2.0 + 4.0 * 40);
    CHECK(onPhraseLine(plan.outBeat, 2.0));
    CHECK(plan.inBeat == 2.0);
    app::AutoInput none;
    const auto missing = app::planAutoTransition(none, input(in, 21));
    CHECK_FALSE(missing.ok);
    CHECK_FALSE(missing.reasons.empty());
}

TEST_CASE("Auto transition: the owner's MIX OUT and MIX IN win (P6-08)", "[app][transition][auto][mixpoints]")
{
    const auto out = make(64, 8, 56, 64);
    const auto in = make(48, 16, 48, 48);
    auto a = input(out, 21);
    auto b = input(in, 21);
    const double first = app::firstDownbeatBeat(out.grid);
    a.mixOutBeat = first + 4.0 * 48; // bar 49: 16 bars before the end
    b.mixInBeat = app::firstDownbeatBeat(in.grid) + 4.0 * 8;
    const auto plan = app::planAutoTransition(a, b);
    REQUIRE(plan.ok);
    CHECK(plan.outBeat == first + 4.0 * 48);
    CHECK(plan.inBeat == app::firstDownbeatBeat(in.grid) + 32.0);
    CHECK(plan.bars <= 16); // it fits before the end
    CHECK(plan.bars >= 4);
    CHECK(plan.swapBar < static_cast<float>(plan.bars));
    CHECK(std::any_of(plan.reasons.begin(), plan.reasons.end(), [](const auto& r) { return r.find("MIX OUT") != std::string::npos; }));
    CHECK(std::any_of(plan.reasons.begin(), plan.reasons.end(), [](const auto& r) { return r.find("MIX IN") != std::string::npos; }));
    // Near the end: as long as fits (4 bars at least).
    a.mixOutBeat = first + 4.0 * 62;
    const auto late = app::planAutoTransition(a, b);
    REQUIRE(late.ok);
    CHECK(late.outBeat == first + 4.0 * 62);
    CHECK(late.bars == 4);
}
