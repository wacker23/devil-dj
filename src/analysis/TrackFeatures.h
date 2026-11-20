#pragma once

#include "util/Beatgrid.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace devildj::analysis
{

// Track Matcher features v1 (P2-24; MATCHER_SPEC §4.3-4.5), our own DSP, from one pass over the mono 22.05 kHz
// signal (1024-point FFT every 256 samples, ~86 frames per second):
//
//   Energy: per 16-bar window (64 beats of the grid; 30 s windows without one) the short-term loudness (dB), the
//   onset rate (onsets per second), the low-band energy ratio (< 150 Hz) and the spectral centroid (Hz). These raw
//   window values are stored; the 0..1 energy is the percentile of
//       raw = 0.35 z(loudness) + 0.25 z(onset rate) + 0.20 z(low ratio) + 0.20 z(centroid)
//   among all windows of the owner's library, z over the library (FeatureNormalizer, recomputed after imports).
//
//   Sound vector (32-d): mean and standard deviation of MFCC 1-13 (26 mel bands, 20 Hz - 8 kHz), mean spectral
//   centroid, roll-off (85 %) and flatness, the onset rate, the low (< 150 Hz) and high (> 2.5 kHz) energy ratios.
//   Raw; the matcher z-normalizes it with the library's statistics.
//
//   Groove (48-d): the average accent (spectral flux) on each of the 16 sixteenth-note positions of a bar, in three
//   bands - low < 150 Hz (kick), mid 150 Hz - 2.5 kHz (snare), high > 5 kHz (hats) - each band centred and
//   scaled to unit length, the whole to unit length. Needs a beatgrid (bars start on its downbeats).
struct EnergyWindow
{
    float startSeconds = 0.0f, seconds = 0.0f;
    float loudnessDb = -100.0f; // mean power of the window, dBFS
    float onsetRate = 0.0f;     // onsets per second
    float lowRatio = 0.0f;      // energy below 150 Hz / all
    float centroidHz = 0.0f;    // power-weighted spectral centroid
};

inline constexpr int kSoundDims = 32;
inline constexpr int kGrooveDims = 48;
inline constexpr int kFeatureVersion = 1; // track_features.version

struct TrackFeatures
{
    std::vector<EnergyWindow> windows;
    std::optional<std::array<float, kSoundDims>> sound;
    std::optional<std::array<float, kGrooveDims>> groove;
};

// `grid` in the mono signal's samples (an empty grid: 30 s energy windows, no groove).
TrackFeatures computeTrackFeatures(const float* mono, int numSamples, double sampleRate, const util::Beatgrid& grid);

// Library statistics of the four energy inputs (mean, standard deviation over every window).
struct EnergyStats
{
    std::array<double, 4> mean{}, sd{1.0, 1.0, 1.0, 1.0};
};
EnergyStats energyStats(std::span<const EnergyWindow> windows);
double rawEnergy(const EnergyWindow& window, const EnergyStats& stats);

// Cosine similarity (0 when either is all zeros).
double cosine(std::span<const float> a, std::span<const float> b);

// Storage: float32 arrays as little-endian bytes; energy windows as 6 float32 each; float16 for the energy curve.
std::vector<std::uint8_t> packFloats(std::span<const float> values);
std::vector<float> unpackFloats(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> packWindows(std::span<const EnergyWindow> windows);
std::vector<EnergyWindow> unpackWindows(std::span<const std::uint8_t> bytes);
std::uint16_t toHalf(float value);
float fromHalf(std::uint16_t half);
std::vector<std::uint8_t> packHalves(std::span<const float> values);
std::vector<float> unpackHalves(std::span<const std::uint8_t> bytes);

} // namespace devildj::analysis
