#pragma once

// Piano-roll editing on the model (P4-07): scales, chord stamps, snapping, quantize, finding notes. Pure functions.

#include "beatmaker/Model.h"

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace devildj::beatmaker
{

struct Scale
{
    const char* name;
    std::vector<int> steps; // semitones from the root
};
// 0 = off ("Chromatic"); then major, minor, dorian, mixolydian, harmonic minor, pentatonics, blues.
const std::vector<Scale>& scales();
bool inScale(int pitch, int root, int scale);
// The nearest pitch in the scale (ties go down); the pitch itself with the scale off.
int snapToScale(int pitch, int root, int scale);
// One scale step up or down from `pitch` (a semitone with the scale off).
int stepInScale(int pitch, int root, int scale, int direction);

struct Chord
{
    const char* name;
    std::vector<int> intervals;
};
// 0 = single note; then maj, min, 7, maj7, min7, sus2, sus4.
const std::vector<Chord>& chords();
std::vector<int> chordPitches(int root, int chord); // within 0..127

// The snap grid: 1/4, 1/8, 1/16, 1/32 (ticks of 96 per beat), as triplets (x 2/3) if asked.
int snapTicks(int division, bool triplet); // division: 4, 8, 16 or 32
int snapDown(int tick, int grid);
int snapNearest(int tick, int grid);

// Every note's start to the nearest grid line (its length kept, at least one grid if it was shorter); notes landing
// on the same start and pitch merge (the longer kept). `only`: just those note indices.
void quantize(SynthTrack& track, int grid, const std::vector<std::size_t>* only = nullptr);
// The note sounding at (tick, pitch), if any (the latest-starting one).
std::optional<std::size_t> noteAt(const SynthTrack& track, int tick, int pitch);
// Adds notes (sorted in), returns the index of the first one added.
std::size_t addNotes(SynthTrack& track, const std::vector<Note>& notes);
void sortNotes(SynthTrack& track, std::optional<std::size_t>* follow = nullptr);

} // namespace devildj::beatmaker
