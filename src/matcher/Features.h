#pragma once

#include "matcher/Camelot.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace devildj::matcher
{

// The Track Matcher's features (MATCHER_SPEC §4): each a value in [0, 1] and a short reason; no value = missing data
// (the scorer skips it and renormalizes the weights).
struct Feature
{
    std::optional<double> value;
    std::string reason;
};

enum class Intent
{
    Down,
    Hold,
    Up,
    Switch,
};

// §4.1 Tempo fit. aBpmNow: A as heard; bBpm: B's own; the best of m = 1/2, 1, 2. `stretch` gets s (B's rate change).
Feature tempoFit(double aBpmNow, double bBpm, double* stretch = nullptr, double* multiple = nullptr);
// §3.3: can B be stretched to A within maxStretch (default 8 %) at some m?
bool tempoReachable(double aBpmNow, double bBpm, double maxStretch = 0.08);

// §4.2 Harmonic fit. With key lock off on the target deck B's key moves by 12 log2(1 + s) semitones (rounded);
// with Key Shift allowed, B a semitone down / up is tried at a 0.1 cost and the best kept.
Feature keyFit(int keyA, int keyB, const HarmonicTable& table, double stretch = 0.0, bool keyLockOff = false,
               bool allowShift = false);

// §4.3 Energy flow (energies 0..1; shown as 1..10).
Feature energyFit(std::optional<double> energyA, std::optional<double> energyB, Intent intent);
int energyLevel(double energy) noexcept; // 1..10

// §4.4 Sound similarity: cosine of the (library-normalized) sound vectors, calibrated by the library's 10th / 90th
// percentile of pairwise similarity; inverted for intent switch (the scorer halves its weight then).
double cosine(std::span<const float> a, std::span<const float> b) noexcept;
Feature soundFit(std::span<const float> a, std::span<const float> b, double c10, double c90, Intent intent);

// §4.5 Groove similarity: cosine of the 48-d groove profiles, mapped to [0, 1].
Feature grooveFit(std::span<const float> a, std::span<const float> b);

// §4.8 Tags and genre: Jaccard overlap (case-insensitive).
Feature tagsFit(const std::vector<std::string>& a, const std::vector<std::string>& b);

// §4.9 Personal history: g(n) = log(1 + n) / log(1 + nMax).
Feature historyFit(int nPair, int nArtistPair, int nTagPair, int nMax);

// §4.10 Rating (0 = unrated -> 0.5) and freshness (plays in the last 30 days against the most played).
Feature ratingFit(int rating);
Feature freshnessFit(int playsLast30Days, int maxPlays);

} // namespace devildj::matcher
