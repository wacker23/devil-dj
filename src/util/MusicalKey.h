#pragma once

#include <cmath>
#include <cstdlib>
#include <optional>
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

// ---- Key Sync and Key Shift (P2-22) ----

// The key `semitones` up (+) or down (-), same mode.
inline int transposeKey(int key, int semitones) noexcept
{
    if (!isValidKey(key))
        return key;
    return (key >= 12 ? 12 : 0) + ((key % 12 + semitones) % 12 + 12) % 12;
}

// 1..12 on the Camelot wheel (C major 8B, A minor 8A); 0 for no key.
inline int camelotNumber(int key) noexcept
{
    if (!isValidKey(key))
        return 0;
    const int majorPitch = key >= 12 ? (key % 12 + 3) % 12 : key;
    return (7 * majorPitch + 7) % 12 + 1;
}

// Mix without clashing: the same Camelot number (the same key or its relative major / minor), or one step round the
// wheel in the same mode (a fifth up or down).
inline bool harmonicallyCompatible(int a, int b) noexcept
{
    if (!isValidKey(a) || !isValidKey(b))
        return false;
    const int na = camelotNumber(a), nb = camelotNumber(b);
    if (na == nb)
        return true;
    const int step = ((nb - na) % 12 + 12) % 12;
    return (a >= 12) == (b >= 12) && (step == 1 || step == 11);
}

// The pitch change a tempo makes without key lock, in whole semitones (12 log2 rate, rounded).
inline int semitonesFromRate(double rate) noexcept
{
    return rate > 0.0 ? static_cast<int>(std::lround(12.0 * std::log2(rate))) : 0;
}

// The key a deck is heard in: the track's key, moved by Key Shift (key lock on) or by its tempo (off).
inline int heardKey(int key, bool keyLock, int keyShift, double tempoRate) noexcept
{
    return transposeKey(key, keyLock ? keyShift : semitonesFromRate(tempoRate));
}

// Key Sync: the smallest Key Shift (-6..+6; at a tie, down) that puts `deckKey` on the master's Camelot number -
// the master's key or its relative - or, with `compatible`, on any key compatible with it. nullopt without keys.
inline std::optional<int> keySyncShift(int deckKey, int masterKey, bool compatible = false) noexcept
{
    if (!isValidKey(deckKey) || !isValidKey(masterKey))
        return std::nullopt;
    for (int distance = 0; distance <= 6; ++distance)
        for (const int s : {-distance, distance})
        {
            const int k = transposeKey(deckKey, s);
            if (compatible ? harmonicallyCompatible(k, masterKey) : camelotNumber(k) == camelotNumber(masterKey))
                return s;
        }
    return std::nullopt;
}

} // namespace devildj::util
