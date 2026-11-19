#pragma once

#include <string>

namespace devildj::util
{

// Key numbers (ARCHITECTURE §7): 0..11 = C major .. B major by pitch class, 12..23 = C minor .. B minor.
// Display as a musical key ("F#m") or a Camelot code ("11A"); the user picks (P2-15).
enum class KeyNotation
{
    Musical,
    Camelot,
};

inline bool isValidKey(int key) noexcept
{
    return key >= 0 && key <= 23;
}

inline std::string musicalKeyName(int key)
{
    static const char* const names[] = {"C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    if (!isValidKey(key))
        return {};
    return std::string(names[key % 12]) + (key >= 12 ? "m" : "");
}

// Camelot wheel: C major = 8B, A minor = 8A; a fifth up = +1.
inline std::string camelotCode(int key)
{
    if (!isValidKey(key))
        return {};
    const bool minor = key >= 12;
    const int pitch = key % 12;
    // Majors: C=8, G=9, D=10 ... ; a minor key shares the number of its relative major (3 semitones up).
    const int majorPitch = minor ? (pitch + 3) % 12 : pitch;
    const int number = (7 * majorPitch + 7) % 12 + 1;
    return std::to_string(number) + (minor ? "A" : "B");
}

inline std::string keyName(int key, KeyNotation notation)
{
    return notation == KeyNotation::Camelot ? camelotCode(key) : musicalKeyName(key);
}

// Related keys, for evaluating key detection (P2-15): the relative major / minor, a fifth up or down (same mode),
// the parallel key.
inline int relativeKey(int key) noexcept
{
    return key >= 12 ? (key % 12 + 3) % 12 : 12 + (key + 9) % 12;
}
inline bool isFifthApart(int a, int b) noexcept
{
    if ((a >= 12) != (b >= 12))
        return false;
    const int d = ((b % 12) - (a % 12) + 12) % 12;
    return d == 7 || d == 5;
}

} // namespace devildj::util
