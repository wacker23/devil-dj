#pragma once

// The beat-FX interface (P3-08; ARCHITECTURE §3.8). An effect produces its wet signal; the slot around it
// (dsp::fx::FxSlot) does dry / wet, on / off and the tail. Pure DSP: no JUCE, no decks.

namespace devildj::dsp::fx
{

// The master clock as an effect needs it for beat-synced times, per block (from engine::MasterClock).
struct BeatContext
{
    double bpm = 120.0;              // the master tempo as heard
    double beatAtBlockStart = 0.0;   // the master beat at the block's first sample
    double beatsPerSample = 0.0;     // bpm / (60 x sample rate)
};

// A parameter, normalised 0..1 (the effect maps it to its own range).
struct EffectParam
{
    const char* name = "";
    float defaultValue = 0.0f;
};

class Effect
{
public:
    virtual ~Effect() = default;

    virtual const char* name() const noexcept = 0;

    // Not real-time safe: allocate here (enough for the longest beat length at the slowest tempo).
    virtual void prepare(double sampleRate, int maxBlockSize) = 0;
    // Real-time safe: clear the state (delay lines, envelopes), keep the parameters.
    virtual void reset() noexcept = 0;
    // Real-time safe. In: the signal sent to the effect; out (in place): the effect's wet output only.
    virtual void process(float* left, float* right, int numSamples, const BeatContext& beat) noexcept = 0;

    // True when its output goes on after its input stops (echo, reverb, ...): switched off, the slot stops
    // sending to it but keeps it running until the tail has died away. False: the slot fades it out (20 ms).
    virtual bool hasTail() const noexcept { return false; }

    // Beat length in beats (1/16 ... 16) for beat-synced effects.
    virtual void setBeats(double beats) noexcept { (void)beats; }

    // Parameters: the first is the one knob a slot shows in multi mode; single mode shows them all.
    virtual int numParams() const noexcept { return 0; }
    virtual EffectParam param(int index) const noexcept
    {
        (void)index;
        return {};
    }
    virtual void setParam(int index, float value) noexcept
    {
        (void)index;
        (void)value;
    }
};

} // namespace devildj::dsp::fx
