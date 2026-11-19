#pragma once

#include <string>
#include <vector>

namespace devildj::analysis
{

// Beat tracker v1 (P2-13), classic DSP, our own:
//   spectral-flux onset envelope (log-magnitude STFT, 1024 / hop 128 at 22.05 kHz = 172 frames per second)
//   -> tempo from a comb-filter tempogram on the envelope's autocorrelation, 70-180 BPM, a prior that prefers
//      85-175 BPM against octave errors
//   -> beat positions by dynamic programming (each beat close to one period after the last, on strong onsets)
//   -> a constant-tempo grid fitted to the beat times by least squares (outliers dropped) - the BPM comes from
//      this fit over the whole track, so it is much finer than the tempogram's step
//   -> the downbeat: the bar phase (of 4) whose beats have the strongest onsets, low frequencies weighted up
//   -> variable tempo: local fits over 16-beat windows; flagged when one drifts more than 1 % from the grid.
struct OnsetEnvelope
{
    double frameRate = 0.0;      // frames per second
    double frameOffsetSeconds = 0.0; // time of frame 0
    std::vector<float> flux;     // all bands, normalized (mean 0 removed locally, std 1, >= 0)
    std::vector<float> lowFlux;  // < 200 Hz, same scale
};

OnsetEnvelope onsetEnvelope(const float* mono, int numSamples, double sampleRate);

struct BeatGrid
{
    bool valid = false;           // false: no steady beat found (silence, ambient, too short)
    double bpm = 0.0;
    double firstBeatSeconds = 0.0; // the first beat of the track (an onset, not the grid extended to 0)
    int firstBeatInBar = 1;       // 1..4: where the first beat sits in its bar (1 = a downbeat)
    bool variableTempo = false;   // local tempo drifts more than 1 %
    double maxDriftPercent = 0.0;
    int beatsUsed = 0;            // beats the grid was fitted to
    double tempogramBpm = 0.0;    // the coarse estimate before the fit (diagnostics)
};

struct BeatTrackerParams
{
    double minBpm = 70.0, maxBpm = 180.0;
    double preferMin = 85.0, preferMax = 175.0;
};

BeatGrid trackBeats(const float* mono, int numSamples, double sampleRate, const BeatTrackerParams& params = {});

// beatgrids.anchors_json for a grid: one anchor at the first beat ([{"beat":0,"sample":...,"bpm":...,
// "bar_beat":...}]); `trackSampleRate` is the file's rate (positions are in its samples).
std::string beatgridJson(const BeatGrid& grid, double trackSampleRate);

} // namespace devildj::analysis
