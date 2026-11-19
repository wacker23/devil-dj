// P2-07: smart playlists. Done when: rule -> SQL generation unit tests.

#include "library/Connection.h"
#include "library/Database.h"
#include "library/SmartRules.h"
#include "library/TagRepository.h"
#include "library/TrackRepository.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <set>
#include <type_traits>
#include <variant>

using namespace devildj::library;
namespace fs = std::filesystem;
using F = RuleField;
using O = RuleOp;

namespace
{
constexpr std::int64_t kNow = 1763500000;
constexpr std::int64_t kDay = 86400;

SqlCondition sql(std::vector<Rule> rules, bool all = true)
{
    auto r = toSql({all, std::move(rules)}, kNow);
    REQUIRE(r.isOk());
    return r.value();
}

Rule text(F f, O o, std::string t)
{
    return {f, o, std::move(t)};
}

Rule num(F f, O o, double v, double to = 0.0)
{
    return {f, o, {}, v, to};
}

std::vector<SqlParam> params(std::initializer_list<SqlParam> p)
{
    return p;
}
} // namespace

TEST_CASE("Smart rules -> SQL: text fields", "[library][smart]")
{
    auto c = sql({text(F::Title, O::Contains, "night")});
    CHECK(c.sql == "((t.title LIKE ? ESCAPE '\\'))");
    CHECK(c.params == params({std::string("%night%")}));

    c = sql({text(F::Artist, O::NotContains, "owl")});
    CHECK(c.sql == "((coalesce(t.artist, '') NOT LIKE ? ESCAPE '\\'))");
    CHECK(c.params == params({std::string("%owl%")}));

    c = sql({text(F::Album, O::StartsWith, "After")});
    CHECK(c.sql == "((t.album LIKE ? ESCAPE '\\'))");
    CHECK(c.params == params({std::string("After%")}));

    c = sql({text(F::Genre, O::Is, "Deep House")});
    CHECK(c.sql == "((t.genre = ? COLLATE NOCASE))");
    CHECK(c.params == params({std::string("Deep House")}));

    c = sql({text(F::Genre, O::IsNot, "Techno")});
    CHECK(c.sql == "((coalesce(t.genre, '') <> ? COLLATE NOCASE))");

    CHECK(sql({text(F::Comment, O::IsEmpty, "")}).sql == "((coalesce(t.comment, '') = ''))");
    CHECK(sql({text(F::Comment, O::IsNotEmpty, "")}).sql == "((coalesce(t.comment, '') <> ''))");
    CHECK(sql({text(F::Comment, O::IsEmpty, "")}).params.empty());

    // LIKE wildcards in the text are matched literally.
    c = sql({text(F::Title, O::Contains, "100%_a\\b")});
    CHECK(c.params == params({std::string("%100\\%\\_a\\\\b%")}));
}

TEST_CASE("Smart rules -> SQL: numbers, keys, colours, dates, tags", "[library][smart]")
{
    auto c = sql({num(F::Bpm, O::Between, 128.0, 120.0)}); // reversed range is put in order
    CHECK(c.sql == "((t.bpm BETWEEN ? AND ?))");
    CHECK(c.params == params({120.0, 128.0}));

    c = sql({num(F::Bpm, O::GreaterThan, 125.5)});
    CHECK(c.sql == "((t.bpm > ?))");
    CHECK(c.params == params({125.5}));

    c = sql({num(F::Key, O::Is, 21)}); // whole numbers stay whole
    CHECK(c.sql == "((t.musical_key = ?))");
    CHECK(c.params == params({std::int64_t{21}}));
    c = sql({num(F::Key, O::IsNot, 21)});
    CHECK(c.sql == "(((t.musical_key IS NULL OR t.musical_key <> ?)))"); // unknown key counts as "not Am"

    c = sql({num(F::Colour, O::Is, 0xE03030)});
    CHECK(c.params == params({std::int64_t{0xE03030}}));

    c = sql({num(F::Time, O::LessThan, 330.0)}); // seconds -> the column's milliseconds
    CHECK(c.sql == "((t.duration_ms < ?))");
    CHECK(c.params == params({std::int64_t{330000}}));

    c = sql({num(F::Rating, O::LessThan, 3)});
    CHECK(c.params == params({std::int64_t{3}}));
    CHECK(sql({num(F::Rating, O::IsEmpty, 0)}).sql == "((coalesce(t.rating, 0) = 0))"); // 0 stars = unrated
    CHECK(sql({num(F::Bpm, O::IsEmpty, 0)}).sql == "((t.bpm IS NULL))");
    CHECK(sql({num(F::Year, O::IsNotEmpty, 0)}).sql == "((t.year IS NOT NULL))");

    c = sql({num(F::DateAdded, O::InLast, 30)});
    CHECK(c.sql == "((t.date_added >= ?))");
    CHECK(c.params == params({kNow - 30 * kDay}));
    c = sql({num(F::LastPlayed, O::NotInLast, 7)}); // never played counts as not played lately
    CHECK(c.sql == "((coalesce(t.last_played, 0) < ?))");
    CHECK(c.params == params({kNow - 7 * kDay}));

    c = sql({text(F::Tag, O::Is, "Warm-up")});
    CHECK(c.sql == "((EXISTS (SELECT 1 FROM track_tags tt JOIN tags g ON g.id = tt.tag_id WHERE tt.track_id = t.id "
                   "AND g.name = ? COLLATE NOCASE)))");
    CHECK(c.params == params({std::string("Warm-up")}));
    CHECK(sql({text(F::Tag, O::IsNot, "x")}).sql.rfind("((NOT EXISTS", 0) == 0);
}

TEST_CASE("Smart rules -> SQL: all / any, no rules, refusals", "[library][smart]")
{
    const std::vector<Rule> two{num(F::Bpm, O::Between, 120, 128), text(F::Genre, O::Is, "House")};
    auto all = sql(two, true);
    CHECK(all.sql == "((t.bpm BETWEEN ? AND ?) AND (t.genre = ? COLLATE NOCASE))");
    CHECK(all.params == params({120.0, 128.0, std::string("House")})); // in "?" order
    auto any = sql(two, false);
    CHECK(any.sql == "((t.bpm BETWEEN ? AND ?) OR (t.genre = ? COLLATE NOCASE))");

    CHECK(sql({}).sql == "1"); // no rules: the whole collection
    CHECK(sql({}).params.empty());

    // An operator that doesn't fit the field is refused, not guessed.
    for (const auto& bad : {num(F::Bpm, O::Contains, 1), num(F::Key, O::Between, 1, 2), text(F::Title, O::GreaterThan, "a"),
                            num(F::DateAdded, O::Is, 3), text(F::Tag, O::Contains, "a")})
    {
        INFO(fieldName(bad.field) << " " << opName(bad.op));
        CHECK_FALSE(toSql({true, {bad}}, kNow).isOk());
    }

    // Every operator the editor offers for a field works.
    for (const auto f : {F::Title, F::Artist, F::Album, F::Genre, F::Comment, F::Bpm, F::Key, F::Rating, F::Colour,
                         F::Year, F::Time, F::DateAdded, F::LastPlayed, F::PlayCount, F::Tag})
        for (const auto o : operatorsFor(f))
        {
            INFO(fieldName(f) << " " << opName(o));
            CHECK(toSql({true, {{f, o, "x", 1.0, 2.0}}}, kNow).isOk());
        }
}

TEST_CASE("Smart rules: JSON round trip and bad JSON", "[library][smart]")
{
    SmartRules r;
    r.matchAll = false;
    r.rules = {num(F::Bpm, O::Between, 120.5, 128), text(F::Title, O::Contains, "say \"hi\" \\ \xED\x95\x9C"),
               num(F::DateAdded, O::InLast, 30), num(F::Key, O::Is, 21), text(F::Comment, O::IsEmpty, "")};
    const auto json = toJson(r);
    CHECK(json.rfind("{\"match\":\"any\",\"rules\":[{\"field\":\"bpm\",\"op\":\"between\",\"value\":120.5,\"to\":128}",
                     0) == 0);
    const auto back = parseRules(json);
    REQUIRE(back.isOk());
    CHECK(back.value() == r);

    // Whitespace, escapes and key order don't matter.
    const auto other = parseRules(R"( { "rules" : [ { "op":"is", "text":"\u00e9t\u00e9", "field":"genre" } ], "match":"all" } )");
    REQUIRE(other.isOk());
    REQUIRE(other.value().rules.size() == 1);
    CHECK(other.value().rules[0].text == "\xC3\xA9t\xC3\xA9");
    CHECK(other.value().matchAll);
    CHECK(parseRules("{}").isOk());

    for (const char* bad : {"", "[", "{\"rules\":[{\"field\":\"bpm\"}]}", "{\"rules\":[{\"field\":\"loudness\",\"op\":\"is\"}]}",
                            "{\"rules\":[{\"field\":\"bpm\",\"op\":\"near\"}]}", "{\"match\":\"some\"}",
                            "{\"rules\":5}", "{\"a\":1} trailing"})
    {
        INFO(bad);
        CHECK_FALSE(parseRules(bad).isOk());
    }
}

TEST_CASE("Smart rules: the SQL selects the right tracks", "[library][smart]")
{
    const auto dir = fs::temp_directory_path() /
                     ("devildj-smart-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto db = std::move(Database::open(dir / "library.db")).value();
    std::vector<std::int64_t> ids;
    db->writeAndWait(
        [&](Connection& c)
        {
            struct T
            {
                const char* title;
                const char* genre;
                std::optional<double> bpm;
                std::optional<int> key;
                int rating;
                std::int64_t added;
                std::optional<std::int64_t> played;
                std::int64_t ms;
            };
            for (const T& d : {T{"Midnight 100% Drive", "Deep House", 122.0, 21, 4, kNow - 2 * kDay, kNow - kDay, 361000},
                               T{"Paper Lanterns", "Melodic House", 124.0, 16, 5, kNow - 40 * kDay, std::nullopt, 402000},
                               T{"Glass Ocean", "deep house", 130.0, std::nullopt, 0, kNow - 10 * kDay, kNow - 20 * kDay, 298000},
                               T{"Untitled", "", std::nullopt, 9, 0, kNow, std::nullopt, 0}})
            {
                Track t;
                t.path = std::string("C:\\") + d.title + ".mp3";
                t.contentHash = t.path;
                t.title = d.title;
                t.genre = d.genre;
                t.bpm = d.bpm;
                t.musicalKey = d.key;
                t.rating = d.rating;
                t.dateAdded = d.added;
                t.lastPlayed = d.played;
                if (d.ms > 0)
                    t.durationMs = d.ms;
                ids.push_back(TrackRepository(c).add(t).value());
            }
            TagRepository tags(c);
            CHECK(tags.assign(ids[1], tags.createTag("Warm-up", std::nullopt).value()));
        });
    auto reader = std::move(db->openReader()).value();

    const auto run = [&](std::vector<Rule> rules, bool all = true)
    {
        const auto c = toSql({all, std::move(rules)}, kNow);
        REQUIRE(c.isOk());
        auto* s = reader->prepare("SELECT t.id FROM tracks t WHERE " + c.value().sql + " ORDER BY t.id");
        REQUIRE(s != nullptr);
        for (std::size_t i = 0; i < c.value().params.size(); ++i)
            std::visit(
                [&](const auto& v)
                {
                    if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string>)
                        s->bind(static_cast<int>(i) + 1, std::string_view(v));
                    else
                        s->bind(static_cast<int>(i) + 1, v);
                },
                c.value().params[i]);
        std::set<int> out;
        while (s->step())
            for (int i = 0; i < 4; ++i)
                if (s->getInt(0) == ids[static_cast<std::size_t>(i)])
                    out.insert(i);
        CHECK(s->ok());
        return out;
    };

    CHECK(run({num(F::Bpm, O::Between, 120, 125)}) == std::set<int>{0, 1});
    CHECK(run({text(F::Genre, O::Is, "Deep House")}) == std::set<int>{0, 2}); // ignoring case
    CHECK(run({text(F::Genre, O::Contains, "house")}) == std::set<int>{0, 1, 2});
    CHECK(run({text(F::Title, O::Contains, "100%")}) == std::set<int>{0}); // % literal, not "anything"
    CHECK(run({text(F::Title, O::Contains, "0%")}) == std::set<int>{0});
    CHECK(run({text(F::Genre, O::IsEmpty, "")}) == std::set<int>{3});
    CHECK(run({num(F::Key, O::IsNot, 21)}) == std::set<int>{1, 2, 3});
    CHECK(run({num(F::Rating, O::GreaterThan, 3)}) == std::set<int>{0, 1});
    CHECK(run({num(F::Rating, O::IsEmpty, 0)}) == std::set<int>{2, 3});
    CHECK(run({num(F::DateAdded, O::InLast, 30)}) == std::set<int>{0, 2, 3});
    CHECK(run({num(F::LastPlayed, O::InLast, 7)}) == std::set<int>{0});
    CHECK(run({num(F::LastPlayed, O::NotInLast, 7)}) == std::set<int>{1, 2, 3});
    CHECK(run({num(F::Time, O::GreaterThan, 360)}) == std::set<int>{0, 1}); // 6:00
    CHECK(run({text(F::Tag, O::Is, "warm-up")}) == std::set<int>{1});
    CHECK(run({text(F::Tag, O::IsNot, "Warm-up")}) == std::set<int>{0, 2, 3});
    CHECK(run({num(F::Bpm, O::GreaterThan, 125), text(F::Tag, O::Is, "Warm-up")}, false) == std::set<int>{1, 2});
    CHECK(run({num(F::Bpm, O::GreaterThan, 120), text(F::Genre, O::Contains, "deep")}, true) == std::set<int>{0, 2});
    CHECK(run({}) == std::set<int>{0, 1, 2, 3});

    reader.reset();
    db.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
}
