#pragma once

#include <array>
#include <optional>

namespace devildj::analysis
{

// Integrated loudness after ITU-R BS.1770-4 (P2-16), our own: each channel K-weighted (a high shelf, then a high
// pass), mean squares over 400 ms blocks every 100 ms, loudness l = -0.691 + 10 log10(sum of the channels' mean
// squares) (left / right weight 1), an absolute gate at -70 LUFS and a relative gate 10 LU under the loudness of
// the blocks above it. Offline; for the analysis threads.

// One biquad, normalized (a0 = 1).
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

// The K-weighting filters at a sample rate: the stage-1 high shelf (+4 dB above ~1.7 kHz, the head) and the
// stage-2 high pass (~38 Hz). Derived from the analog design by the bilinear transform; at 48 kHz they equal the
// coefficients printed in BS.1770.
std::array<Biquad, 2> kWeighting(double sampleRate);

// LUFS of a stereo (or mono: right == nullptr) signal; nullopt when no block passes the gates (silence) or the
// signal is shorter than one block.
std::optional<double> integratedLoudness(const float* left, const float* right, int numSamples, double sampleRate);

// Auto Gain (P2-16): the trim that brings a track to the target loudness, within the trim's range.
double autoGainDb(double trackLufs, double targetLufs, double maxDb = 12.0);

} // namespace devildj::analysis
