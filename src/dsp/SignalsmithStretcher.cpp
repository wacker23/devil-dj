#include "dsp/Stretcher.h"

#include <signalsmith-stretch/signalsmith-stretch.h>

namespace devildj::dsp
{

namespace
{
class SignalsmithStretcher final : public Stretcher
{
public:
    SignalsmithStretcher()
        : stretch(12345) // fixed seed: deterministic output for tests
    {
    }

    void prepare(int numChannels, double sampleRate) override
    {
        channels = numChannels;
        stretch.presetDefault(numChannels, static_cast<float>(sampleRate), true);
        stretch.setTransposeSemitones(static_cast<float>(transpose));
    }

    void reset() override { stretch.reset(); }

    int inputLatency() const override { return stretch.inputLatency(); }
    int outputLatency() const override { return stretch.outputLatency(); }
    int seekLength() const override { return stretch.blockSamples() + stretch.intervalSamples(); }

    void seek(const float* const* inputs, int inputSamples, double playbackRate) override
    {
        stretch.seek(inputs, inputSamples, playbackRate);
    }

    void process(const float* const* inputs, int inputSamples, float* const* outputs, int outputSamples) override
    {
        stretch.process(inputs, inputSamples, outputs, outputSamples);
    }

    void setTransposeSemitones(double semitones) override
    {
        if (semitones == transpose)
            return;
        transpose = semitones;
        stretch.setTransposeSemitones(static_cast<float>(semitones));
    }

private:
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    int channels = 2;
    double transpose = 0.0;
};
} // namespace

std::unique_ptr<Stretcher> makeSignalsmithStretcher()
{
    return std::make_unique<SignalsmithStretcher>();
}

} // namespace devildj::dsp
