// P4-09: song mode + live switching. Done when: switching is sample-accurate on the bar.

#include "beatmaker/DrumKits.h"
#include "beatmaker/Model.h"
#include "beatmaker/Sequencer.h"
#include "engine/Scheduler.h"
#include "ui/PadModes.h"
#include "ui/StepGrid.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace devildj;
using namespace devildj::beatmaker;
using util::Fraction;

namespace
{
constexpr double kRate = 48000.0;

// Pattern p: every 16th on, its drum track named after it - so each event says which pattern it came from.
Project patterns()
{
    Project p;
    for (int i = 0; i < kPatternCount; ++i)
    {
        auto& pat = p.patterns[static_cast<std::size_t>(i)];
        pat.drums.push_back(drumTrack("kick", 16));
        for (auto& s : pat.drums[0].steps)
            s.on = true;
    }
    p.patterns[2].lengthSteps = 32; // C: 8 beats
    setPatternLength(p.patterns[2], 32);
    for (auto& s : p.patterns[2].drums[0].steps)
        s.on = true;
    return p;
}

struct Hit
{
    int pattern;
    std::int64_t sample;
    int step;
};

// A producer (25 ms ticks, 100 ms look-ahead) at `bpm`; `act(nowBeat)` may queue switches as it goes.
template <typename Act>
std::vector<Hit> run(const Project& project, Sequencer& seq, double bpm, double untilBeat, Act act)
{
    std::vector<Hit> hits;
    engine::ClockSnapshot clock{0, 0.0, bpm, kRate};
    engine::ScheduleCursor cursor;
    for (std::int64_t now = 0; clock.beatAt(static_cast<double>(now)) < untilBeat; now += 1200)
    {
        const double nowBeat = clock.beatAt(static_cast<double>(now));
        act(nowBeat, cursor.position());
        const auto w = cursor.next(nowBeat, clock.beatAt(static_cast<double>(now + 4800)));
        std::vector<BeatEvent> out;
        seq.emit(project, w.begin, w.end, out);
        for (const auto& e : out)
            hits.push_back({e.pattern, toScheduled(e, clock).sampleTime, e.step});
        seq.prune(w.begin);
    }
    return hits;
}

std::int64_t sampleOfBeat(double beat, double bpm)
{
    return std::llround(beat * 60.0 * kRate / bpm);
}
} // namespace

TEST_CASE("Live switching: a pattern queued mid-bar starts on the next bar's exact sample, at 120 and 174 BPM",
          "[beatmaker][switch]")
{
    for (const double bpm : {120.0, 174.0})
    {
        const auto project = patterns();
        Sequencer seq;
        seq.playPattern(0, 0);
        bool queued = false;
        // At beat 5.3 (mid bar 2) ask for pattern B: the switch is at beat 8, wherever the windows are.
        const auto hits = run(project, seq, bpm, 16.0,
                              [&](double nowBeat, std::optional<Fraction> cursorAt)
                              {
                                  if (!queued && nowBeat >= 5.3)
                                  {
                                      queued = true;
                                      const auto at = Sequencer::nextBar(cursorAt.value_or(Fraction(0)));
                                      CHECK(at == Fraction(8));
                                      seq.queuePattern(1, at);
                                  }
                              });
        const auto bar = sampleOfBeat(8.0, bpm);
        std::int64_t lastA = -1, firstB = -1;
        int aAfter = 0;
        for (const auto& h : hits)
        {
            if (h.pattern == 0)
            {
                lastA = std::max(lastA, h.sample);
                aAfter += h.sample >= bar;
            }
            if (h.pattern == 1 && (firstB < 0 || h.sample < firstB))
                firstB = h.sample;
        }
        INFO(bpm << " BPM");
        CHECK(firstB == bar);                              // B's step 0 exactly on the bar
        CHECK(aAfter == 0);                                // nothing of A from there
        CHECK(lastA == sampleOfBeat(7.75, bpm));           // A's last 16th just before
        CHECK(std::count_if(hits.begin(), hits.end(), [](const Hit& h) { return h.pattern == 0; }) == 32);
        // No 16th missing or doubled across the switch.
        std::vector<std::int64_t> samples;
        for (const auto& h : hits)
            if (h.sample < sampleOfBeat(16.0, bpm)) // the look-ahead also reaches past beat 16
                samples.push_back(h.sample);
        std::sort(samples.begin(), samples.end());
        CHECK(std::adjacent_find(samples.begin(), samples.end()) == samples.end());
        CHECK(static_cast<int>(samples.size()) == 64); // 16 beats of 16ths
    }
}

TEST_CASE("Live switching: at the playing pattern's end; song mode; the playing position and what is queued",
          "[beatmaker][switch]")
{
    const auto project = [] {
        auto p = patterns();
        p.song = {0, 2, 1};
        return p;
    }();
    Sequencer seq;
    seq.playPattern(2, 4); // C (8 beats) from beat 4
    // At its end: C runs 4..12, 12..20 - asked at 13: the switch is at 20.
    CHECK(seq.patternEnd(project, 13) == Fraction(20));
    CHECK(seq.patternEnd(project, 12) == Fraction(12));
    CHECK(Sequencer::nextBar(Fraction(13)) == Fraction(16));
    CHECK(Sequencer::nextBar(Fraction(16)) == Fraction(16));
    seq.queuePattern(0, seq.patternEnd(project, 13));
    CHECK(seq.queuedAfter(13)->pattern == 0);
    CHECK(seq.queuedAfter(13)->at == Fraction(20));
    CHECK_FALSE(seq.queuedAfter(20));
    CHECK(seq.position(project, 19).pattern == 2);
    CHECK(seq.position(project, 19).step == 28); // beat 7 of C, 4 steps a beat
    CHECK(seq.position(project, 20).pattern == 0);
    CHECK(seq.position(project, 20).step == 0);
    CHECK(seq.position(project, 3).pattern == -1); // before it started
    // A new queue replaces what was planned from then on.
    seq.queuePattern(1, 16);
    CHECK(seq.queuedAfter(13)->pattern == 1);
    CHECK(seq.position(project, 21).pattern == 1);
    // Song mode from beat 24: A (4), C (8), B (4), round again.
    seq.queueSong(24);
    std::vector<BeatEvent> out;
    seq.emit(project, 24, 56, out);
    std::vector<std::pair<int, Fraction>> starts;
    for (const auto& e : out)
        if (e.step == 0)
            starts.push_back({e.pattern, e.beat});
    const std::vector<std::pair<int, Fraction>> expected{{0, 24}, {2, 28}, {1, 36}, {0, 40}, {2, 44}, {1, 52}};
    CHECK(starts == expected);
    CHECK(seq.position(project, 30).pattern == 2);
    CHECK(seq.position(project, 37).pattern == 1);
    CHECK(seq.patternEnd(project, 30) == Fraction(36)); // the song pattern's end
    // Prune keeps what plays now.
    seq.prune(30);
    CHECK(seq.position(project, 30).pattern == 2);
    CHECK(seq.songMode());
}

TEST_CASE("Song editor and switch keys on the grid; the PATTERNS pad mode", "[beatmaker][switch][ui]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto project = defaultProject();
    project.patterns[0].drums = kitTracks(drumKits()[0]);
    ui::StepGrid grid;
    grid.setBounds(0, 0, 1400, 420);
    grid.setProject(&project);
    using GA = ui::GlobalAction;
    bool song = false, atEnd = false;
    grid.onSongMode = [&](bool on) { song = on; };
    grid.onSwitchAt = [&](bool on) { atEnd = on; };
    grid.perform(GA::BeatMakerSongAdd);
    grid.perform(GA::BeatMakerPattern2);
    grid.perform(GA::BeatMakerSongAdd);
    grid.perform(GA::BeatMakerSongAdd);
    CHECK(project.song == std::vector<int>{0, 1, 1});
    CHECK(grid.songText() == "song: A B B");
    grid.perform(GA::BeatMakerSongRemove);
    CHECK(project.song == std::vector<int>{0, 1});
    grid.perform(GA::BeatMakerSongMode);
    CHECK(song);
    grid.songButton().onClick();
    CHECK_FALSE(song);
    grid.perform(GA::BeatMakerSwitchAt);
    CHECK(atEnd);
    CHECK(grid.switchAtButton().getButtonText() == "AT END");
    grid.songAddButton().onClick();
    grid.songRemoveButton().onClick();
    CHECK(project.song == std::vector<int>{0, 1});
    grid.setPatternStates(0, 1);
    juce::Image image(juce::Image::ARGB, 1400, 420, true);
    juce::Graphics g(image);
    grid.paintEntireComponent(g, false);
    CHECK(juce::String(ui::padModeName(ui::PadMode::BeatPatterns)) == "PATTERNS");
    CHECK(ui::padModeAvailable(ui::PadMode::BeatPatterns));
    CHECK(ui::kPadModeCount == 10);
}
