#pragma once

// Editing a track's phrases (P6-06): the waveform's right-click menu. Plain functions on the stored list
// (seconds), so they can be tested without the UI. The lengths in bars always come from the grid afterwards.

#include "library/PhraseRepository.h"
#include "util/Beatgrid.h"

#include <vector>

namespace devildj::app
{

using Phrases = std::vector<library::StoredPhrase>;

// The downbeat nearest to `sample` (the grid's samples at `rate`), in seconds.
double nearestBarSeconds(const util::Beatgrid& grid, double sample, double rate);
// The phrase whose start is nearest to `seconds`, within `maxGap` seconds; -1 if none.
int nearestPhrase(const Phrases& phrases, double seconds, double maxGap);

// A phrase starting at `seconds` (nothing if one already does); it takes the energy of the phrase it splits.
Phrases addPhrase(Phrases phrases, double seconds);
// Phrase `index` starts at `seconds` instead (nothing if another one starts there).
Phrases movePhrase(Phrases phrases, int index, double seconds);
// Phrase `index` gone: joined to the one before (energy weighted by bars); the first is just dropped.
Phrases removePhrase(Phrases phrases, int index);
// Sorted by start, each one's bars counted on the grid up to the next start (the last: to `endSample`).
Phrases withBars(Phrases phrases, const util::Beatgrid& grid, double rate, double endSample);

} // namespace devildj::app
