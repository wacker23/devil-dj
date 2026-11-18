#pragma once

#include <memory>

namespace devildj::dsp
{

// Time-stretch / pitch-shift engine behind key lock (P1-08; ARCHITECTURE §4 "Stretcher").
//
// Streaming use: each call to process() takes `inputSamples` of source audio and produces `outputSamples`;
// the ratio is the playback rate, and the pitch stays put (plus any transpose). The stretcher delays audio:
// input fed now is centred `inputLatency()` samples later, and output lags the processing point by
// `outputLatency()` output samples. Callers compensate so the reported position matches what is heard.
//
// prepare() allocates; everything else must be real-time safe (no allocation, no locks).
class Stretcher
{
public:
    virtual ~Stretcher() = default;

    virtual void prepare(int channels, double sampleRate) = 0;
    virtual void reset() = 0;

    virtual int inputLatency() const = 0;
    virtual int outputLatency() const = 0;
    // How many input samples seek() wants to fully prime the stretcher.
    virtual int seekLength() const = 0;

    // Primes the stretcher with input that ends where streaming continues (after reset or a jump).
    virtual void seek(const float* const* inputs, int inputSamples, double playbackRate) = 0;
    virtual void process(const float* const* inputs, int inputSamples, float* const* outputs, int outputSamples) = 0;

    // Pitch shift in semitones (Key Shift / Key Sync, P2-22). 0 = original key.
    virtual void setTransposeSemitones(double semitones) = 0;
};

// Signalsmith Stretch (MIT), default preset with split computation so the FFT work is spread evenly over
// audio blocks instead of arriving in bursts.
std::unique_ptr<Stretcher> makeSignalsmithStretcher();

} // namespace devildj::dsp
