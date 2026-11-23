#pragma once

// The Auto transition (P4-16): where to mix and how, chosen by analysis and rules - not a trained model - with its
// reasons. Later (Phase 6) it can learn from the owner's edits.
//
// 1. Each track's bar profile from its waveform and grid: low / mid / high level per bar (dB).
// 2. Phrase lines: every 4 bars from the first downbeat, scored by how much the level changes there (8 / 16 bars
//    count a little more).
// 3. The outgoing track's outro: the last phrase line (in its last 45 %) where the bass drops out and stays out;
//    none: it ends at full level. Its end: the last bar with sound.
//    The incoming track's intro: the first phrase line (in its first 45 %) where its bass comes in; none: it
//    starts with its bass.
// 4. Timing: the incoming track's bass entry lands where the outgoing track's bass leaves - the bass swap there,
//    the transition from (outro - intro) to the outgoing track's end (4 .. 32 bars, on phrase lines).
// 5. The preset: keys that clash -> short and filter-led; a big tempo gap or no outro -> echo out; a long intro and
//    matching keys -> a long blend; more energy coming -> filter rise; else bass swap / smooth blend.

#include "analysis/WaveformBuilder.h"
#include "app/TransitionPresets.h"
#include "engine/TransitionCurves.h"
#include "util/Beatgrid.h"

#include <string>
#include <vector>

namespace devildj::app
{

struct BarLevels
{
    float low = -100.0f, mid = -100.0f, high = -100.0f; // dB (RMS)
    float total() const noexcept;
};

// Bar b = the grid's beats [4b, 4b + 4) from its first downbeat on (bars before it are left out).
std::vector<BarLevels> barProfile(const analysis::Waveform& waveform, const util::Beatgrid& grid, int numSamples);

struct TrackShape
{
    int bars = 0;
    int introEnd = 0;  // the bar its bass comes in (0: from the start)
    int outroStart = 0; // the bar its bass leaves for good (= contentEnd: no outro)
    int contentEnd = 0; // one past the last bar with sound
    double firstDownbeatBeat = 0.0; // the grid beat of bar 0
};
TrackShape trackShape(const std::vector<BarLevels>& bars, double firstDownbeatBeat);

struct AutoInput
{
    const analysis::Waveform* waveform = nullptr;
    util::Beatgrid grid;
    int numSamples = 0;
    int key = -1;      // util::MusicalKey (-1 unknown)
    double bpm = 0.0;  // the track's own tempo
};

struct AutoPlan
{
    bool ok = false;
    double outBeat = 0.0; // the outgoing track's grid beat where the transition starts
    double inBeat = 0.0;  // the incoming track's grid beat it starts from (its first downbeat, usually)
    int bars = 8;
    TransitionPreset preset = TransitionPreset::SmoothBlend;
    float swapBar = 4.0f; // bars into the transition where the basses swap
    engine::TransitionCurves curves;
    std::vector<std::string> reasons;
    TrackShape outgoing, incoming;
};

AutoPlan planAutoTransition(const AutoInput& outgoing, const AutoInput& incoming);

// The first downbeat of a grid at or after sample 0, as a grid beat.
double firstDownbeatBeat(const util::Beatgrid& grid);

} // namespace devildj::app
