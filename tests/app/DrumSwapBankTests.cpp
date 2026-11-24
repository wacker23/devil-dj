// P6-05: Drum Swap banks kept across runs, and the drums captured from the bar.

#include "app/DrumSwapBank.h"
#include "engine/StemLoader.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using devildj::app::captureDrums;
using devildj::app::DrumSwapBank;
using devildj::app::DrumSwapSlot;
using namespace devildj;

namespace
{
struct TempFolder
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("devildj-drumswap", "");
    ~TempFolder() { dir.deleteRecursively(); }
};

std::shared_ptr<engine::TrackBuffer> sine(int n, double rate, double hz)
{
    auto t = std::make_shared<engine::TrackBuffer>();
    t->sampleRate = rate;
    t->audio.setSize(2, n);
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < 2; ++c)
            t->audio.setSample(c, i, static_cast<float>(0.5 * std::sin(2.0 * 3.141592653589793 * hz * i / rate)));
    return t;
}
} // namespace

TEST_CASE("Drum Swap bank: slots, single / multi and the capture length survive a restart", "[app][drumswap]")
{
    TempFolder t;
    {
        DrumSwapBank bank(t.dir);
        CHECK(bank.load(48000.0) == 0);
        CHECK(bank.slot(0, 0).empty());
        DrumSwapSlot s;
        s.name = "CAP 8";
        s.beats = 8.0;
        s.audio = sine(48000, 48000.0, 100.0);
        CHECK(bank.set(1, 3, s).isEmpty());
        s.name = "BEAT A";
        s.beats = 4.0;
        CHECK(bank.set(0, 0, s).isEmpty());
        bank.setMulti(1, true);
        bank.setCaptureBeats(16);
        CHECK(bank.deckFolder(1).getChildFile("slot-4.wav").existsAsFile());
    }
    DrumSwapBank again(t.dir);
    CHECK(again.load(48000.0) == 2);
    CHECK(again.slot(1, 3).name == "CAP 8");
    CHECK(again.slot(1, 3).beats == 8.0);
    REQUIRE_FALSE(again.slot(1, 3).empty());
    CHECK(again.slot(1, 3).audio->numSamples() == 48000);
    CHECK(std::abs(again.slot(1, 3).audio->audio.getSample(0, 120) - std::sin(2.0 * 3.141592653589793 * 100.0 * 120 / 48000.0) * 0.5) < 1e-4);
    CHECK(again.slot(0, 0).name == "BEAT A");
    CHECK(again.multi(1));
    CHECK_FALSE(again.multi(0));
    CHECK(again.captureBeats() == 16);

    SECTION("at another device rate the loops are resampled (same length in time)")
    {
        DrumSwapBank at44(t.dir);
        CHECK(at44.load(44100.0) == 2);
        CHECK(at44.slot(1, 3).audio->sampleRate == 44100.0);
        CHECK(at44.slot(1, 3).audio->numSamples() == 44100);
    }
    SECTION("clear removes the file and the entry")
    {
        CHECK(again.clear(1, 3).isEmpty());
        CHECK_FALSE(again.deckFolder(1).getChildFile("slot-4.wav").existsAsFile());
        DrumSwapBank third(t.dir);
        CHECK(third.load(48000.0) == 1);
        CHECK(third.slot(1, 3).empty());
    }
}

TEST_CASE("Drum Swap: capture takes the drums stem from the bar at or before the playhead", "[app][drumswap]")
{
    engine::StemBuffer stems;
    stems.length = 10 * 48000;
    stems.sampleRate = 48000.0;
    for (int s = 0; s < 4; ++s)
        for (int c = 0; c < 2; ++c)
            stems.samples[static_cast<std::size_t>(s * 2 + c)].assign(static_cast<std::size_t>(stems.length), 0);
    for (int i = 0; i < stems.length; ++i) // the drums: their own sample number (mod 30000), so the start shows
        for (int c = 0; c < 2; ++c)
            stems.samples[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] = static_cast<std::int16_t>(i % 30000);
    // 120 BPM, the first downbeat at 1000: bars at 1000, 97000, ...
    const auto grid = util::Beatgrid::constant(1000.0, 120.0, 48000.0);
    const auto loop = captureDrums(stems, grid, 150000.0, 4); // in bar 2 (97000 .. 193000)
    REQUIRE(loop);
    const auto& a = *loop.value();
    CHECK(a.numSamples() == 96000);
    CHECK(a.sampleRate == 48000.0);
    CHECK(a.audio.getSample(0, 0) == static_cast<float>(97000 % 30000) / 32768.0f);
    CHECK(a.audio.getSample(1, 10) == static_cast<float>((97000 + 10) % 30000) / 32768.0f);

    CHECK_FALSE(captureDrums(stems, grid, 400000.0, 32)); // past the end
    CHECK_FALSE(captureDrums(stems, util::Beatgrid(), 1000.0, 4)); // no grid
}
