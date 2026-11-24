#pragma once

// Smart cues (P6-07): hot cue suggestions at phrase starts, learnt from the owner's own cues.
//
// Candidates: every phrase start (P6-06), with what makes it a cue point - the first downbeat, an energy rise
// (the biggest one: the main drop), an energy fall (a break), the outro (the last fall in the last third).
// Each gets a rule score; once the owner has cued enough tracks a logistic regression on the candidates'
// features (where in the track, energy before / after / change, which phrase from the start / end, its length)
// learns which ones the owner actually cues, and the score blends the two. The best (up to 8, at least
// kMinScore) are offered on empty pads in time order. Pure functions; the app feeds the library's data in.

#include "library/PhraseRepository.h"

#include <array>
#include <string>
#include <vector>

namespace devildj::app
{

enum class CueReason
{
    Start,  // the first downbeat
    Rise,   // energy goes up
    Drop,   // the biggest rise: the main drop
    Break,  // energy goes down
    Outro,  // the last fall in the last third
    Phrase, // a phrase start, nothing special
};
const char* cueReasonName(CueReason reason) noexcept; // "START", "RISE", "DROP", "BREAK", "OUTRO", "PHRASE"
CueReason cueReasonFromName(const std::string& name) noexcept;

constexpr int kCueFeatures = 9;
using CueFeatureVector = std::array<double, kCueFeatures>;

struct CueCandidate
{
    double seconds = 0.0;
    CueReason reason = CueReason::Phrase;
    CueFeatureVector features{};
    double rule = 0.0;  // 0..1
    double score = 0.0; // what's used: the rule, or rule and habit blended
};

// Every phrase start of a track as a candidate (phrases by start; `duration` in seconds).
std::vector<CueCandidate> cueCandidates(const std::vector<library::StoredPhrase>& phrases, double duration);

// The owner's habit: logistic regression on candidate features (positive = the owner cued there).
class CueHabits
{
public:
    struct Example
    {
        CueFeatureVector features;
        bool cued = false;
    };
    static constexpr int kMinEach = 12; // positives and negatives needed before it counts

    void train(const std::vector<Example>& examples);
    bool trained() const noexcept { return ready; }
    double probability(const CueFeatureVector& f) const noexcept;
    int positives() const noexcept { return pos; }
    int negatives() const noexcept { return neg; }

private:
    CueFeatureVector weights{};
    double bias = 0.0;
    bool ready = false;
    int pos = 0, neg = 0;
};

// Labels a track's candidates by the owner's hot cues there (seconds; within `tolerance`) for training.
std::vector<CueHabits::Example> cueExamples(const std::vector<CueCandidate>& candidates,
                                            const std::vector<double>& cuedSeconds, double tolerance);

constexpr double kMinScore = 0.35;
// The suggestions for a track: scored (blended with the habits once trained), not within `tolerance` of a cue
// already set or one the owner rejected, the best `maxCount` at kMinScore or more, in time order.
std::vector<CueCandidate> suggestCues(const std::vector<library::StoredPhrase>& phrases, double duration,
                                      const CueHabits& habits, const std::vector<double>& existing,
                                      const std::vector<double>& rejected, double tolerance, int maxCount = 8);

// Suggestions on the empty pads of a page (slots 0..7): in time order on the empty slots from A. Returns, per
// slot, the suggestion index or -1.
std::array<int, 8> padsForSuggestions(const std::array<bool, 8>& slotSet, int count);

// The training examples from the library (any thread with its own connection): every track with phrases that the
// owner has hot cues on or has answered suggestions for. A candidate within `tolerance` of a hot cue (or an
// accepted suggestion) is cued; a rejected one is not.
std::vector<CueHabits::Example> gatherCueExamples(library::Connection& db, double tolerance = 1.0);

// The acceptance rate the owner's answers give (0 when there are none).
struct CueAnswers
{
    int accepted = 0, rejected = 0;
    double rate() const noexcept { return accepted + rejected > 0 ? static_cast<double>(accepted) / (accepted + rejected) : 0.0; }
};

} // namespace devildj::app
