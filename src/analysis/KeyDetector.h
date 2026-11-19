#pragma once

#include <array>

namespace devildj::analysis
{

// Key detection (P2-15), our own: a chromagram (8192-point FFT every 2048 samples at 22.05 kHz, magnitudes mapped
// to 36 bins per octave from C2 to C7), the tuning estimated from where the energy sits between semitones and
// taken out before the 36 bins fold to 12 pitch classes; the frames summed (louder frames count more); then the
// correlation with 24 key profiles (Krumhansl-Kessler major / minor tone profiles) rotated to every root.
struct KeyEstimate
{
    bool valid = false;
    int key = -1;            // 0..23 (util/MusicalKey.h)
    double confidence = 0.0; // 0..1: how much the best key stands out from the second
    double correlation = 0.0; // of the best key
    double tuningCents = 0.0; // the track's tuning against A = 440 Hz
    std::array<double, 12> chroma{}; // the track's pitch-class profile (C..B), sums to 1
    std::array<double, 24> scores{}; // every key's correlation
};

KeyEstimate detectKey(const float* mono, int numSamples, double sampleRate);

} // namespace devildj::analysis
