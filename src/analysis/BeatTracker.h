#pragma once

#include <string>
#include <vector>

namespace devildj::analysis
{

// Beat tracker (P2-13, v2 after the eval set), classic DSP, our own:
//   spectral-flux onset envelope (log-magnitude STFT, 1024 / hop 128 at 22.05 kHz = 172 frames per second)
//   -> tempo from a comb-filter tempogram on the envelope's autocorrelation, 70-200 BPM, a prior that prefers
//      85-175 BPM against octave errors
//   -> beat positions by dynamic programming (each beat close to one period after the last, on strong onsets)
//   -> a constant-tempo grid fitted to the beat times by least squares, the outlier gate tightening pass by pass
//   -> steady or live: the beats split into straight pieces; when most sit in pieces at one exact tempo the track
//      is steady (the odd pieces are slips of the beat chain) and the constant grid is refitted on the beats in
//      phase with them; otherwise, when a tempo-following grid (anchors where the tempo changes, at most 16)
//      keeps clearly more of the beats within 25 ms than the constant one, the grid follows the band
//   -> the downbeat: the bar phase (of 4) whose beats have the strongest onsets, low frequencies weighted up.
struct OnsetEnvelope
{
    double frameRate = 0.0;      // frames per second
    double frameOffsetSeconds = 0.0; // time of frame 0
    std::vector<float> flux;     // all bands, normalized (mean 0 removed locally, std 1, >= 0)
    std::vector<float> lowFlux;  // < 200 Hz, same scale
};

OnsetEnvelope onsetEnvelope(const float* mono, int numSamples, double sampleRate);

// Where the tempo changes in a variable-tempo grid: from this beat on (0 = the first beat), at this time.
struct BeatAnchor
{
    int beat = 0;
    double seconds = 0.0;
    double bpm = 0.0;
};

struct BeatGrid
{
    bool valid = false;           // false: no steady beat found (silence, ambient, too short)
    double bpm = 0.0;
    double firstBeatSeconds = 0.0; // the first beat of the track (an onset, not the grid extended to 0)
    int firstBeatInBar = 1;       // 1..4: where the first beat sits in its bar (1 = a downbeat)
    bool variableTempo = false;   // a tempo-following grid fits clearly better: `anchors` follow the tempo
    double maxDriftPercent = 0.0; // local tempo (16-beat windows) against the average
    double constantFraction = 0.0; // share of the beats a constant grid fits (diagnostics)
    double constantHitRate = 0.0;  // share of the detected beats within 25 ms of the constant grid
    double followHitRate = 0.0;    // ... of the tempo-following grid (the anchors)
    double steadyShare = 0.0;      // share of the beats in straight pieces at one tempo (diagnostics)
    std::vector<BeatAnchor> anchors; // variable tempo: where the tempo changes (at most 16); empty = constant
    int beatsUsed = 0;            // beats the grid was fitted to
    double tempogramBpm = 0.0;    // the coarse estimate before the fit (diagnostics)
};

struct BeatTrackerParams
{
    double minBpm = 70.0, maxBpm = 200.0;
    double preferMin = 85.0, preferMax = 175.0;
    // Variable tempo (anchors) when the constant grid keeps fewer of the beats on it than variableBelowHits and the
    // tempo-following grid keeps at least variableAboveGain more.
    double variableBelowHits = 0.5, variableAboveGain = 0.12;
    // A steady track (constant grid, refitted on the steady pieces) when this share of the beats sits in pieces at
    // one tempo.
    double steadyAboveShare = 0.4;
    // Beat-chain tightness for a second try on tracks that aren't steady (the first uses 400).
    double looseTightness = 100.0;
};

BeatGrid trackBeats(const float* mono, int numSamples, double sampleRate, const BeatTrackerParams& params = {});

// beatgrids.anchors_json for a grid: one anchor at the first beat ([{"beat":0,"sample":...,"bpm":...,
// "bar_beat":...}]); `trackSampleRate` is the file's rate (positions are in its samples).
std::string beatgridJson(const BeatGrid& grid, double trackSampleRate);

} // namespace devildj::analysis
