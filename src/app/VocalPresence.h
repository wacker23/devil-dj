#pragma once

// Vocal presence (P6-09): where a track has vocals, from its vocal stem (P6-03's cache).
//
//   The curve: the vocal stem's level (dB RMS, both channels) per 0.1 s window, cached as vocal.json beside the
//   stems (made once, from vocals.flac).
//   Active: a window above both an absolute floor (-40 dBFS) and 18 dB under the track's loud vocals (its 95th
//   percentile window) - so the bleed a separation leaves in the vocal stem, and an instrumental, read as none.
//   Per phrase (P6-06): the share of its windows that are active, 0..1.
//   vocal_intro / vocal_outro (track_features): the first and the last phrase's.
//
// File I/O: loader / background threads; the rest pure.

#include "library/PhraseRepository.h"

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

namespace devildj::app
{

struct VocalCurve
{
    static constexpr double kWindowSeconds = 0.1;
    std::vector<float> db; // per window from the start of the audio
    bool empty() const noexcept { return db.empty(); }
};

// From a vocals.flac (any rate); nullopt if it can't be read.
std::optional<VocalCurve> vocalCurveFromFile(const juce::File& vocalsFlac);
// The stems folder's vocal.json, made from its vocals.flac first if it isn't there.
std::optional<VocalCurve> ensureVocalCurve(const juce::File& stemsFolder);
bool saveVocalCurve(const VocalCurve& curve, const juce::File& json);
std::optional<VocalCurve> loadVocalCurve(const juce::File& json);

// Which windows are active (see above).
std::vector<bool> activeWindows(const VocalCurve& curve);
// Per phrase, 0..1 (the last phrase runs to the end of the curve).
std::vector<double> vocalPerPhrase(const VocalCurve& curve, const std::vector<library::StoredPhrase>& phrases);

struct VocalEnds
{
    double intro = 0.0, outro = 0.0;
};
std::optional<VocalEnds> vocalEnds(const std::vector<double>& perPhrase);

// track_features.vocal_intro / vocal_outro (a row made if the track has none yet; the other columns left alone).
bool storeVocalEnds(library::Connection& db, std::int64_t trackId, const VocalEnds& ends);

} // namespace devildj::app
