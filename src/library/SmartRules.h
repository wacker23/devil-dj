#pragma once

#include "util/Result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace devildj::library
{

// Smart playlists (P2-07): rules on track fields, all or any of them must match. Stored as JSON in
// playlists.rule_json and turned into an SQL condition on `tracks t` when the playlist is shown, so the list is
// always up to date.
enum class RuleField
{
    Title,
    Artist,
    Album,
    Genre,
    Comment,
    Bpm,
    Key,        // 0..23 (TagReader.h parseKeyTag)
    Rating,     // 0..5 stars
    Colour,     // 0xRRGGBB
    Year,
    Time,       // track length in seconds
    DateAdded,  // "in the last N days"
    LastPlayed, // "in the last N days"
    PlayCount,
    Tag,        // a tag name
};

enum class RuleOp
{
    Contains,
    NotContains,
    Is,
    IsNot,
    StartsWith,
    GreaterThan,
    LessThan,
    Between,
    InLast,    // days (DateAdded, LastPlayed)
    NotInLast, // days; never played counts as "not in the last N days"
    IsEmpty,
    IsNotEmpty,
};

struct Rule
{
    RuleField field = RuleField::Title;
    RuleOp op = RuleOp::Contains;
    std::string text;            // text fields and Tag
    double value = 0.0, to = 0.0; // numbers (Between uses both; key / colour / rating / days in value)
    friend bool operator==(const Rule&, const Rule&) = default;
};

struct SmartRules
{
    bool matchAll = true; // AND; false = OR
    std::vector<Rule> rules;
    friend bool operator==(const SmartRules&, const SmartRules&) = default;
};

using SqlParam = std::variant<std::int64_t, double, std::string>;

struct SqlCondition
{
    std::string sql; // e.g. "((t.bpm BETWEEN ? AND ?) AND (t.genre = ? COLLATE NOCASE))"
    std::vector<SqlParam> params;
};

// Which operators make sense for a field (the rule editor offers these; toSql refuses the others).
std::vector<RuleOp> operatorsFor(RuleField field);
bool isTextField(RuleField field) noexcept;
const char* fieldName(RuleField field) noexcept; // JSON / UI name: "bpm", "date_added", ...
const char* opName(RuleOp op) noexcept;          // "contains", "in_last", ...

// The SQL condition (on table alias `t`). `nowSeconds` anchors "in the last N days". No rules = every track.
// Fails on a rule whose operator doesn't fit its field.
util::Result<SqlCondition> toSql(const SmartRules& rules, std::int64_t nowSeconds);

// JSON: {"match":"all","rules":[{"field":"bpm","op":"between","value":120,"to":128}, {"field":"genre",
// "op":"is","text":"House"}]}. Unknown fields / operators fail to parse (nothing is silently dropped).
std::string toJson(const SmartRules& rules);
util::Result<SmartRules> parseRules(std::string_view json);

} // namespace devildj::library
