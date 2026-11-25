#pragma once

// The personal ranker v1 (P6-12; MATCHER_SPEC §7.2): a pairwise logistic ranker learnt from the choice log.
//
//   x(A, B): the choice log's 14 values (11 features, 3 intent interactions); a missing value counts 0 in a pair.
//   Pairs: the chosen B against each shown-not-chosen C, weighted by the outcome - mixed / thumbs up 2, a plain
//   choice 1; ejected (0.5) and thumbs down (2) turn the pair round (the others should have won).
//   P(B > C) = sigmoid(w . (x_B - x_C));  loss = - sum weight log P + lambda |w - w_preset|^2, w >= 0 (projected
//   gradient, Adam). lambda keeps it near the preset while data is scarce.
//   The learnt weights are scaled to the preset's sum (a logistic model's scale is arbitrary) and blended:
//   w = (1 - a) w_preset + a w_learnt, a = n / (n + 200), n = impressions.
//
// The 5 random easy negatives of §7.2 are left out: their x isn't in the log (it would need the whole library
// scored at training time).
//
// Stored in matcher_models as "personal-v1". Pure functions + the storage; any thread with its own connection.

#include "matcher/ChoiceLog.h"
#include "matcher/Scorer.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace devildj::library
{
class Connection;
}

namespace devildj::matcher
{

constexpr int kRankDims = kChoiceDims; // 14
using RankWeights = std::array<double, kRankDims>;

RankWeights rankWeightsOf(const Weights& preset); // the preset's 11, interactions 0
Weights weightsOf(const RankWeights& w);           // back, interactions included

struct RankPair
{
    std::array<double, kRankDims> d{}; // x(winner) - x(loser)
    double weight = 1.0;
};
std::vector<RankPair> rankPairs(const std::vector<Impression>& impressions);

struct TrainOptions
{
    double lambda = 20.0; // the preset weighs about as much as 20 pairs
    int iterations = 400;
    double learningRate = 0.05;
};
struct TrainResult
{
    RankWeights weights{}; // scaled to the prior's sum of the 11 features
    int pairs = 0;
    double loss = 0.0;    // per unit of pair weight, at the end
    double seconds = 0.0;
};
TrainResult trainRanker(const std::vector<RankPair>& pairs, const RankWeights& prior, const TrainOptions& options = {});

double blendAlpha(int impressions) noexcept; // n / (n + 200)
Weights blendWeights(const Weights& preset, const RankWeights& learnt, int impressions);

struct PersonalModel
{
    RankWeights weights{};
    int version = 0;
    std::int64_t trainedAt = 0;
    int impressions = 0;
    std::string basedOn; // the preset it started from
};
constexpr const char* kPersonalModelName = "personal-v1";
std::optional<PersonalModel> loadPersonalModel(library::Connection& db);
bool savePersonalModel(library::Connection& db, const PersonalModel& model);
bool resetPersonalModel(library::Connection& db);

// Everything: the log -> pairs -> trained -> saved (version + 1). Returns the model, or none if there's nothing to
// learn from.
std::optional<PersonalModel> retrainPersonalModel(library::Connection& db, const Weights& preset,
                                                  const std::string& presetName, std::int64_t now);

} // namespace devildj::matcher
