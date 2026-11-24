#pragma once

// Phrase detection (P6-06): where a track's phrases start (on its bars), how long they are (8 / 16 / 24 / 32
// bars, 4 when the music says so), and how much energy each one has.
//
//   1. Per bar of the beatgrid (downbeat to downbeat): chroma (12 pitch classes, 80 Hz - 5 kHz) and the level in
//      three bands (below 150 Hz, 150 Hz - 2 kHz, above), plus the onset density (spectral flux).
//   2. A bar-by-bar self-similarity matrix (chroma cosine and band levels, z-scored over the track) and Foote's
//      novelty along its diagonal: a checkerboard kernel 8 bars wide (Gaussian tapered), so a boundary is where the
//      bars before differ from the bars after.
//   3. A DP over the bars picks the boundaries: phrases of 8 / 16 / 24 / 32 bars (4 at a cost), each boundary
//      scored by its novelty less a threshold; the last phrase is what remains.
//   4. Energy per phrase: its loudness against the track's loudest phrase (18 dB range) and its onset density
//      against the busiest, 0..1.
//
// Positions are seconds from the start of the audio (not beats), so phrases stay put if the grid is edited later.
// Pure function of the mono signal and the grid; analysis threads.

#include "util/Beatgrid.h"

#include <vector>

namespace devildj::analysis
{

struct Phrase
{
    double startSeconds = 0.0;
    int bars = 0;
    double energy = 0.0; // 0..1
    friend bool operator==(const Phrase&, const Phrase&) = default;
};

struct PhraseAnalysis
{
    std::vector<Phrase> phrases;
    std::vector<double> barStarts; // seconds: every bar the grid gives inside the audio
    std::vector<double> novelty;   // per bar (a boundary at its start), 0..1
};

struct PhraseParams
{
    int kernelBars = 8;          // the checkerboard's width
    double threshold = 0.22;     // a boundary pays off when its novelty (0..1) is above this
    double shortPenalty = 0.25;  // a 4-bar phrase costs this much more
    double gridBonus = 0.04;     // per boundary that keeps the 8-bar grid from the first bar
};

PhraseAnalysis detectPhrases(const float* mono, int numSamples, double sampleRate, const util::Beatgrid& grid,
                             const PhraseParams& params = {});

// Boundary F-measure (P6-06 evaluation): detected vs reference phrase starts (seconds; the track's first phrase
// start isn't a boundary and is left out by the caller), a hit within `tolerance` seconds, each reference matched
// once.
struct BoundaryScore
{
    int hits = 0, detected = 0, reference = 0;
    double precision() const noexcept { return detected > 0 ? static_cast<double>(hits) / detected : 0.0; }
    double recall() const noexcept { return reference > 0 ? static_cast<double>(hits) / reference : 0.0; }
    double f() const noexcept
    {
        const double p = precision(), r = recall();
        return p + r > 0.0 ? 2.0 * p * r / (p + r) : 0.0;
    }
    BoundaryScore& operator+=(const BoundaryScore& o) noexcept
    {
        hits += o.hits;
        detected += o.detected;
        reference += o.reference;
        return *this;
    }
};
BoundaryScore scoreBoundaries(const std::vector<double>& detected, const std::vector<double>& reference,
                              double tolerance);

} // namespace devildj::analysis
