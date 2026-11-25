#pragma once

// The regions a transition is made of (P6-10; MATCHER_SPEC §1), the same for the analysis, the matcher and the app:
//
//   Mix-in region of a track:  32 beats from its MIX IN, else from its first phrase (its first downbeat), else 0.
//   Mix-out region of a track: the 32 beats before its MIX OUT, else before its outro phrase (the last of two or
//                              more), else before its end.
//
// Times in seconds from the start of the audio. Also the intro / outro length in bars (the first / last phrase).

#include <optional>
#include <vector>

namespace devildj::util
{

struct PhraseSpan
{
    double startSeconds = 0.0;
    int bars = 0;
};

struct MixRegions
{
    double inStart = 0.0, inEnd = 0.0;
    double outStart = 0.0, outEnd = 0.0;
    int introBars = 0, outroBars = 0; // 0: unknown (no phrases)
};

inline MixRegions mixRegions(const std::vector<PhraseSpan>& phrases, std::optional<double> mixIn,
                             std::optional<double> mixOut, double bpm, double duration)
{
    constexpr double kBeats = 32.0;
    const double span = kBeats * 60.0 / (bpm > 0.0 ? bpm : 120.0);
    MixRegions r;
    r.inStart = mixIn ? *mixIn : !phrases.empty() ? phrases.front().startSeconds : 0.0;
    r.inEnd = r.inStart + span;
    r.outEnd = mixOut ? *mixOut : phrases.size() >= 2 ? phrases.back().startSeconds : duration;
    if (r.outEnd <= 0.0)
        r.outEnd = duration;
    r.outStart = r.outEnd > span ? r.outEnd - span : 0.0;
    if (!phrases.empty())
        r.introBars = phrases.front().bars;
    if (phrases.size() >= 2)
        r.outroBars = phrases.back().bars;
    return r;
}

} // namespace devildj::util
