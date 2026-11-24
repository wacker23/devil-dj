#pragma once

// P6-06: generated tracks with known phrases, for the phrase detector's evaluation. Mono at 22.05 kHz, a constant
// grid (the first downbeat a little after the start), sections built like dance tracks: intro (kick, hats),
// build (+ chords, a rising noise), drop (everything: kick, clap, hats, bass, chords, lead), break (chords and lead,
// no drums), outro. Every section has its own chord progression, level and instruments. The hard parts, on
// purpose: hat patterns that change every 4 bars inside a section (decoys), a snare fill in the bar before some
// boundaries, some sections 4 bars longer (off the 8-bar grid), and some boundaries where only the chords change.

#include "util/Beatgrid.h"

#include <cmath>
#include <numbers>
#include <random>
#include <string>
#include <vector>

namespace devildj::test
{

struct PhraseTrack
{
    std::vector<float> mono;
    double rate = 22050.0;
    util::Beatgrid grid;
    std::vector<double> boundaries; // seconds: where each section after the first starts
    std::vector<int> sectionBars;
    std::string description;
};

inline PhraseTrack makePhraseTrack(unsigned seed)
{
    std::mt19937 rng(seed);
    const auto uniform = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
    const auto chance = [&](double p) { return uniform(0.0, 1.0) < p; };
    PhraseTrack t;
    const double bpm = std::round(uniform(118.0, 134.0));
    const double beat = 60.0 / bpm;
    const double start = uniform(0.05, 0.4); // the first downbeat
    t.grid = util::Beatgrid::constant(start * t.rate, bpm, t.rate);

    enum Kind
    {
        Intro,
        Build,
        Drop,
        Break,
        Outro
    };
    struct Section
    {
        Kind kind;
        int bars;
        int progression;
        double level;
        bool fillBefore = false;
    };
    // A plan: intro, (build, drop, break){1..2}, build, drop, outro.
    std::vector<Section> plan;
    const auto len = [&](std::initializer_list<int> options)
    {
        std::vector<int> o(options);
        int bars = o[std::uniform_int_distribution<int>(0, static_cast<int>(o.size()) - 1)(rng)];
        if (chance(0.12))
            bars += 4; // an extension: off the 8-bar grid
        return bars;
    };
    int progression = std::uniform_int_distribution<int>(0, 5)(rng);
    const auto nextProgression = [&](bool same)
    {
        if (!same)
            progression = (progression + 1 + std::uniform_int_distribution<int>(0, 4)(rng)) % 6;
        return progression;
    };
    plan.push_back({Intro, len({16, 32}), nextProgression(false), 0.55});
    const int middles = chance(0.5) ? 2 : 1;
    for (int m = 0; m < middles; ++m)
    {
        plan.push_back({Build, len({8, 16}), nextProgression(false), 0.7});
        plan.push_back({Drop, len({16, 32}), nextProgression(chance(0.3)), 1.0});
        // A second drop half with only new chords (a subtle boundary) now and then.
        if (chance(0.35))
            plan.push_back({Drop, len({8, 16}), nextProgression(false), 1.0});
        plan.push_back({Break, len({8, 16}), nextProgression(false), 0.6});
    }
    plan.push_back({Build, len({8}), nextProgression(false), 0.7});
    plan.push_back({Drop, len({16, 32}), nextProgression(false), 1.0});
    plan.push_back({Outro, len({16, 32}), nextProgression(false), 0.55});
    for (std::size_t i = 1; i < plan.size(); ++i)
        plan[i].fillBefore = chance(0.4);

    int totalBars = 0;
    for (const auto& s : plan)
        totalBars += s.bars;
    const double end = start + totalBars * 4.0 * beat + 2.0;
    t.mono.assign(static_cast<std::size_t>(end * t.rate), 0.0f);

    // Chord progressions (roots in semitones from A, minor / major triads), one chord per bar.
    static const int kRoots[6][4] = {{0, 8, 3, 10}, {0, 5, 8, 7}, {0, 3, 8, 10}, {5, 0, 7, 3}, {0, 10, 8, 7}, {3, 10, 0, 5}};
    const int key = std::uniform_int_distribution<int>(0, 11)(rng);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    const double tuning = 110.0 * std::pow(2.0, key / 12.0);
    const auto add = [&](double at, double seconds, auto&& f)
    {
        const auto a = static_cast<std::size_t>(std::max(0.0, at * t.rate));
        const auto n = static_cast<std::size_t>(seconds * t.rate);
        for (std::size_t i = 0; i < n && a + i < t.mono.size(); ++i)
            t.mono[a + i] += f(static_cast<double>(i) / t.rate);
    };
    double at = start;
    int sectionIndex = 0;
    for (const auto& s : plan)
    {
        if (sectionIndex > 0)
            t.boundaries.push_back(at);
        t.sectionBars.push_back(s.bars);
        t.description += std::to_string(s.bars) + (s.kind == Intro ? "i " : s.kind == Build ? "b " : s.kind == Drop ? "d " : s.kind == Break ? "k " : "o ");
        const bool drums = s.kind != Break;
        const bool bass = s.kind == Drop || (s.kind == Outro && chance(0.5));
        const bool chords = s.kind != Intro && s.kind != Outro;
        const bool lead = s.kind == Drop || s.kind == Break;
        const double g = s.level * 0.25;
        for (int bar = 0; bar < s.bars; ++bar)
        {
            const double barStart = at + bar * 4.0 * beat;
            const int root = kRoots[s.progression][bar % 4];
            const double rootHz = tuning * std::pow(2.0, root / 12.0);
            const bool minor = (s.progression + bar) % 2 == 0;
            const bool lastBar = bar == s.bars - 1;
            const bool fill = lastBar && sectionIndex + 1 < static_cast<int>(plan.size()) &&
                              plan[static_cast<std::size_t>(sectionIndex + 1)].fillBefore;
            const int hatStyle = (bar / 4) % 2; // the decoy: hats change every 4 bars
            for (int b = 0; b < 4; ++b)
            {
                const double t0 = barStart + b * beat;
                if (drums && !(fill && b >= 2))
                    add(t0, 0.25, [&](double x) { return static_cast<float>(g * 1.6 * std::exp(-x * 18.0) * std::sin(2.0 * std::numbers::pi * (45.0 + 120.0 * std::exp(-x * 35.0)) * x)); });
                if (drums && (s.kind == Drop || s.kind == Build) && (b % 2 == 1))
                    add(t0, 0.15, [&](double x) { return static_cast<float>(g * 0.5 * std::exp(-x * 30.0) * noise(rng)); });
                if (drums)
                    for (int h = 0; h < (hatStyle == 0 ? 2 : 4); ++h)
                        if (hatStyle == 1 || h == 1)
                            add(t0 + h * beat / (hatStyle == 0 ? 2.0 : 4.0), 0.04,
                                [&](double x) { return static_cast<float>(g * 0.18 * std::exp(-x * 120.0) * noise(rng)); });
                if (fill && b >= 2) // a snare roll into the next section
                    for (int r = 0; r < 4; ++r)
                        add(t0 + r * beat / 4.0, 0.06, [&](double x) { return static_cast<float>(g * 0.45 * std::exp(-x * 45.0) * noise(rng)); });
                if (bass)
                    for (int e = 0; e < 2; ++e)
                        add(t0 + e * beat / 2.0 + beat / 4.0, beat / 2.2, [&](double x)
                            {
                                const double ph = 2.0 * std::numbers::pi * rootHz / 2.0 * x;
                                return static_cast<float>(g * 0.6 * std::min(1.0, x * 200.0) * std::exp(-x * 3.0) *
                                                          (std::sin(ph) + 0.4 * std::sin(2.0 * ph) + 0.2 * std::sin(3.0 * ph)));
                            });
                if (lead && (b + bar) % 2 == 0)
                    add(t0, beat * 0.9, [&](double x)
                        {
                            const int step = (b + bar * 3) % 3;
                            const double hz = rootHz * 4.0 * std::pow(2.0, (step == 0 ? 0 : step == 1 ? (minor ? 3 : 4) : 7) / 12.0);
                            return static_cast<float>(g * 0.12 * std::exp(-x * 4.0) * std::sin(2.0 * std::numbers::pi * hz * x));
                        });
            }
            if (chords)
                add(barStart, 4.0 * beat, [&](double x)
                    {
                        const double third = minor ? 3.0 : 4.0;
                        double v = 0.0;
                        for (const double semis : {0.0, third, 7.0})
                            v += std::sin(2.0 * std::numbers::pi * rootHz * 2.0 * std::pow(2.0, semis / 12.0) * x);
                        return static_cast<float>(g * 0.09 * std::min(1.0, x * 20.0) * v);
                    });
            if (s.kind == Build) // a rising noise sweep over the build
                add(barStart, 4.0 * beat, [&, bar](double x)
                    { return static_cast<float>(g * 0.05 * (bar + x / (4.0 * beat)) / s.bars * noise(rng)); });
        }
        at += s.bars * 4.0 * beat;
        ++sectionIndex;
    }
    return t;
}

} // namespace devildj::test
