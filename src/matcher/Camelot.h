#pragma once

#include <array>
#include <string>

namespace devildj::matcher
{

// Keys and the Camelot wheel for the Track Matcher (MATCHER_SPEC §4.2). Key numbers as everywhere else
// (util/MusicalKey): 0..11 = C..B major, 12..23 = C..B minor.
int camelotNumber(int key) noexcept; // 1..12 (0 = no key)
char camelotLetter(int key) noexcept; // 'A' minor, 'B' major (0 = no key)
std::string camelotCode(int key);      // "8A"

enum class KeyRelation
{
    Same,             // Δ = 0, same letter                  1.00
    Adjacent,         // |Δ| = 1, same letter (a fifth)      0.90
    Relative,         // Δ = 0, other letter                 0.85
    Diagonal,         // |Δ| = 1, other letter               0.60
    EnergyBoost,      // Δ = +2, same letter                 0.55
    SemitoneUp,       // Δ = -5, same letter (Camelot +7)    0.50
    TwoDown,          // Δ = -2, same letter                 0.45
    SemitoneDown,     // Δ = +5, same letter (Camelot -7)    0.40
    Other,            //                                     0.10
    Count
};

// Δ = number(B) - number(A) wrapped to [-6, 6].
int camelotDelta(int keyA, int keyB) noexcept;
KeyRelation relation(int keyA, int keyB) noexcept;

// The scores per relation (matcher/harmonic_table.json can replace them: {"same": 1.0, "adjacent": 0.9, ...}).
struct HarmonicTable
{
    std::array<double, static_cast<std::size_t>(KeyRelation::Count)> score{1.00, 0.90, 0.85, 0.60, 0.55,
                                                                            0.50, 0.45, 0.40, 0.10};
    double of(KeyRelation r) const noexcept { return score[static_cast<std::size_t>(r)]; }
    static const char* name(KeyRelation r) noexcept; // the JSON names: "same", "adjacent", ...
    // Values from JSON (missing ones keep their defaults); false if it isn't a JSON object.
    bool load(const std::string& json);
};

} // namespace devildj::matcher
