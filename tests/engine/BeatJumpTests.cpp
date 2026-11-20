// P2-21: beat jump. Done when: unit tests; keyboard bindings in KEYMAP.

#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "ui/Actions.h"
#include "ui/KeyboardController.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>

using namespace devildj;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

struct Rig
{
    engine::TrackBuffer track;
    engine::AudioEngine engine;
    float l[kBlock], r[kBlock];
    float* outs[2] = {l, r};
    explicit Rig(const util::Beatgrid& grid, double seconds = 300.0)
    {
        track.sampleRate = kRate;
        track.audio.setSize(2, static_cast<int>(seconds * kRate));
        track.audio.clear();
        engine.prepare(kRate, kBlock);
        engine.send(engine::LoadTrack{0, &track, 1});
        engine.send(engine::SetBeatgrid{0, grid});
        block();
    }
    void block() { engine.process(outs, 2, kBlock); }
    double position() { return engine.readState().decks[0].positionSamples; }
    void seek(double sample)
    {
        engine.send(engine::Seek{0, sample});
        block();
    }
};
} // namespace

TEST_CASE("Beat jump: every size, back and forward, exactly that many beats", "[engine][beatjump]")
{
    const auto grid = util::Beatgrid::constant(0.37 * kRate, 126.0, kRate);
    Rig rig(grid);
    for (const double size : {0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0})
        for (const double direction : {1.0, -1.0})
        {
            INFO(direction * size << " beats");
            const double from = grid.sampleAt(200.0) + 1234.5; // somewhere inside beat 200 (keeps its offset)
            rig.seek(from);
            rig.engine.send(engine::BeatJump{0, direction * size});
            rig.block();
            CHECK(std::abs(rig.position() - grid.sampleAt(grid.beatAt(from) + direction * size)) < 1e-6);
        }
}

TEST_CASE("Beat jump: a grid with tempo changes counts its own beats", "[engine][beatjump]")
{
    auto grid = util::Beatgrid::constant(0.0, 120.0, kRate);
    REQUIRE(grid.addAnchor({16.0, 16 * 24000.0, 140.0, 1})); // 16 beats at 120, then 140
    Rig rig(grid);
    rig.seek(grid.sampleAt(14.0)); // 2 beats before the change: +4 beats = 2 at 120 + 2 at 140
    rig.engine.send(engine::BeatJump{0, 4.0});
    rig.block();
    CHECK(std::abs(rig.position() - (16 * 24000.0 + 2 * 48000.0 * 60.0 / 140.0)) < 1e-6);
}

TEST_CASE("Beat jump while playing with quantize: on the grid point, phase kept; jumps add up", "[engine][beatjump]")
{
    const auto grid = util::Beatgrid::constant(0.5 * kRate, 128.0, kRate);
    const double spb = 60.0 / 128.0 * kRate;
    Rig rig(grid);
    rig.engine.send(engine::SetQuantize{true, 1.0f});
    rig.engine.send(engine::Seek{0, grid.sampleAt(10.3)});
    rig.engine.send(engine::Play{0});
    rig.block();
    // Two jumps pressed before the next beat: +4 and +4 = +8, on beat 11.
    rig.engine.send(engine::BeatJump{0, 4.0});
    rig.engine.send(engine::BeatJump{0, 4.0});
    rig.block();
    CHECK(rig.engine.readState().decks[0].queuedActions == 1);
    double before = rig.position();
    for (int i = 0; i < 100; ++i)
    {
        rig.block();
        const double now = rig.position();
        if (now > before + kBlock + 1.0)
        {
            // It crossed beat 11 in this block and went on from beat 19.
            const double crossing = grid.sampleAt(11.0);
            REQUIRE(before < crossing);
            REQUIRE(before + kBlock >= crossing);
            CHECK(std::abs(now - (grid.sampleAt(19.0) + (before + kBlock - crossing))) <= 1.0);
            break;
        }
        before = now;
    }
    CHECK(rig.engine.readState().decks[0].queuedActions == 0);
    CHECK(grid.beatAt(rig.position()) > 19.0);
    // The phase: a whole number of beats from where it would have been.
    const double fraction = grid.beatAt(rig.position()) - std::floor(grid.beatAt(rig.position()));
    const double fractionWithout =
        grid.beatAt(rig.position() - 8.0 * spb) - std::floor(grid.beatAt(rig.position() - 8.0 * spb));
    CHECK(std::abs(fraction - fractionWithout) * spb < 1.0);

    // Quantize off: at once, from wherever it is.
    rig.engine.send(engine::SetQuantize{false, 1.0f});
    rig.block();
    const double here = rig.position();
    rig.engine.send(engine::BeatJump{0, -2.0});
    rig.block();
    CHECK(std::abs(rig.position() - (grid.sampleAt(grid.beatAt(here) - 2.0) + kBlock)) <= 1.0);
}

TEST_CASE("Beat jump: the track's ends, no grid; the fine 5 ms jump", "[engine][beatjump]")
{
    const auto grid = util::Beatgrid::constant(0.5 * kRate, 120.0, kRate);
    Rig rig(grid, 20.0);
    rig.seek(grid.sampleAt(1.0));
    rig.engine.send(engine::BeatJump{0, -8.0}); // before the start: to the start
    rig.block();
    CHECK(rig.position() == 0.0);
    rig.engine.send(engine::BeatJump{0, 128.0}); // past the end: to the end
    rig.block();
    CHECK(rig.position() == rig.track.audio.getNumSamples());

    rig.seek(5.0 * kRate);
    rig.engine.send(engine::JumpBy{0, 0.005});
    rig.block();
    CHECK(rig.position() == 5.0 * kRate + 240.0);
    rig.engine.send(engine::JumpBy{0, -0.005});
    rig.engine.send(engine::JumpBy{0, -0.005});
    rig.block();
    CHECK(rig.position() == 5.0 * kRate - 240.0);

    // No beatgrid: beats mean nothing, the deck stays.
    rig.engine.send(engine::SetBeatgrid{0, util::Beatgrid()});
    rig.block();
    rig.engine.send(engine::BeatJump{0, 4.0});
    rig.block();
    CHECK(rig.position() == 5.0 * kRate - 240.0);
}

TEST_CASE("Beat jump keys (KEYMAP.md)", "[engine][beatjump][ui][keys]")
{
    ui::ActionDispatcher d;
    ui::KeyboardController k(d);
    namespace scan = ui::scan;
    namespace mods = ui::mods;
    using DA = ui::DeckAction;
    const auto is = [&](std::uint16_t code, std::uint8_t m, ui::CommandId expected)
    {
        const auto got = k.lookup({code, m}, ui::KeyContext::Main);
        INFO(ui::describeChord({code, m}) << " -> " << (got ? ui::commandName(*got) : std::string("nothing")));
        CHECK((got && *got == expected));
    };
    is(scan::Digit5, mods::None, ui::deckCommand(0, DA::BeatJumpBack));
    is(scan::Digit6, mods::None, ui::deckCommand(0, DA::BeatJumpForward));
    is(scan::Minus, mods::None, ui::deckCommand(1, DA::BeatJumpBack));
    is(scan::Equals, mods::None, ui::deckCommand(1, DA::BeatJumpForward));
    is(scan::Digit5, mods::Ctrl, ui::deckCommand(0, DA::FineJumpBack));
    is(scan::Digit6, mods::Ctrl, ui::deckCommand(0, DA::FineJumpForward));
    is(scan::Minus, mods::Ctrl, ui::deckCommand(1, DA::FineJumpBack));
    is(scan::Equals, mods::Ctrl, ui::deckCommand(1, DA::FineJumpForward));
    is(scan::Digit6, mods::Shift, ui::deckCommand(0, DA::BeatJumpSizeUp));
    is(scan::Digit6, mods::Ctrl | mods::Shift, ui::deckCommand(0, DA::BeatJumpSizeDown));
    is(scan::Equals, mods::Shift, ui::deckCommand(1, DA::BeatJumpSizeUp));
    is(scan::Equals, mods::Ctrl | mods::Shift, ui::deckCommand(1, DA::BeatJumpSizeDown));
    CHECK(ui::findConflicts(ui::defaultKeymap()).empty());
}
