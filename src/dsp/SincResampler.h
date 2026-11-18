#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace devildj::dsp
{

// High-quality offline sample-rate conversion (P1-04: tracks are converted to the device rate at load).
//
// Kaiser-windowed sinc interpolation with a precomputed, finely sampled kernel. The low-pass cutoff sits at
// `cutoff` x the lower of the two Nyquist frequencies, so downsampling doesn't alias. Not for the audio
// thread (allocates); real-time variable-rate playback uses dsp/Resampler (P1-07).
struct SincResamplerSettings
{
    int zeroCrossings = 32;    // kernel half-length in zero crossings of the (narrower) sinc
    int phasesPerSample = 512; // kernel table resolution between input samples
    double kaiserBeta = 9.0;   // ~ -90 dB sidelobes
    double cutoff = 0.95;      // fraction of the lower Nyquist frequency kept
};

// Output length for a given input length: round(inputLength * outRate / inRate).
std::int64_t resampledLength(std::int64_t inputLength, double inRate, double outRate) noexcept;

// Resamples one channel. If the rates are equal, returns a copy.
std::vector<float> resample(std::span<const float> input, double inRate, double outRate,
                            const SincResamplerSettings& settings = {});

} // namespace devildj::dsp
