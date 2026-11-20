#pragma once

#include <string>

namespace devildj::library
{
class Connection;
}

namespace devildj::analysis
{

// Library-percentile normalization of the Track Matcher features (P2-24; MATCHER_SPEC §4.3-4.4). Reads every
// track's raw energy windows (track_features.energy_features), z-scores the four inputs over all windows of the
// library, scores each window (rawEnergy) and maps it to its percentile among all windows (0..1). Writes per track
// energy (mean of its windows), energy_intro (first window), energy_outro (last), energy_curve (float16 per window);
// and the library statistics to feature_stats: 'energy' (the z means / sds) and 'sound_mean' / 'sound_sd' (per
// dimension of the raw sound vectors, for the matcher's z-normalization). Run after imports / analysis, on the
// writer thread. Returns the tracks updated, or -1 on an error.
int normalizeLibraryFeatures(library::Connection& db, std::string* error = nullptr);

} // namespace devildj::analysis
