// P6-11: the choice log - x(A, B), the stored impressions, the outcomes a session gives, the exports.

#include "app/ChoiceTracker.h"
#include "library/Connection.h"
#include "library/Database.h"
#include "matcher/ChoiceLog.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace devildj;
using namespace devildj::matcher;

namespace
{
std::array<std::optional<double>, kFeatureCount> features(double base)
{
    std::array<std::optional<double>, kFeatureCount> f{};
    for (int i = 0; i < kFeatureCount; ++i)
        f[static_cast<std::size_t>(i)] = base + 0.01 * i;
    f[static_cast<std::size_t>(FeatureId::Vocal)].reset(); // missing data stays missing
    return f;
}

struct Lib
{
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                ("devildj-choices-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::unique_ptr<library::Database> db = std::move(library::Database::open(dir / "library.db")).value();
    ~Lib()
    {
        db.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

Impression impression(std::int64_t a, std::int64_t b, std::optional<int> chosenRank, int shownCount)
{
    Impression m;
    m.sessionId = 77;
    m.createdAt = 1000 + b;
    m.contextTrack = a;
    m.chosenTrack = b;
    m.intent = Intent::Up;
    m.preset = "Balanced";
    m.scope = "library";
    m.modelVersion = "rules";
    for (int i = 0; i < shownCount; ++i)
    {
        const std::int64_t track = chosenRank && *chosenRank == i + 1 ? b : 100 + i;
        m.shown.push_back({track, i + 1, track == b, choiceVector(features(0.1 * i), 0.05 * i, m.intent)});
    }
    if (!chosenRank)
        m.shown.push_back({b, std::nullopt, true, choiceVector(features(0.5), -0.2, m.intent)});
    m.chosenRank = chosenRank;
    return m;
}
} // namespace

TEST_CASE("Choice log: x(A, B) - the 11 features and the 3 interaction terms", "[app][choicelog]")
{
    const auto f = features(0.4);
    auto x = choiceVector(f, 0.2, Intent::Up);
    REQUIRE(x.size() == 14);
    CHECK(x[0] == 0.4);
    CHECK_FALSE(x[static_cast<std::size_t>(FeatureId::Vocal)]);
    CHECK(x[11] == 0.2); // energy delta x intent up
    CHECK(x[12] == 0.0);
    CHECK(x[13] == 0.0);
    x = choiceVector(f, 0.2, Intent::Down);
    CHECK(x[11] == 0.0);
    CHECK(x[12] == 0.2);
    x = choiceVector(f, std::nullopt, Intent::Switch);
    CHECK_FALSE(x[11]);
    CHECK(x[13] == *f[static_cast<std::size_t>(FeatureId::Sound)]);
    CHECK(std::string(choiceDimName(13)) == "sound_switch");
    CHECK(std::string(choiceDimName(0)) == "tempo");
}

TEST_CASE("Choice log: shown_json round trip (nulls kept)", "[app][choicelog]")
{
    const auto m = impression(1, 2, std::nullopt, 3);
    const auto back = shownFromJson(shownToJson(m.shown));
    REQUIRE(back.size() == 4);
    CHECK(back[0].rank == 1);
    CHECK_FALSE(back[0].chosen);
    CHECK_FALSE(back[3].rank);
    CHECK(back[3].chosen);
    CHECK(back[3].track == 2);
    for (std::size_t i = 0; i < back.size(); ++i)
        for (int k = 0; k < kChoiceDims; ++k)
        {
            const auto& a = m.shown[i].x[static_cast<std::size_t>(k)];
            const auto& b = back[i].x[static_cast<std::size_t>(k)];
            REQUIRE(a.has_value() == b.has_value());
            if (a)
                CHECK(std::abs(*a - *b) < 1e-6);
        }
    CHECK(shownFromJson("garbage").empty());
}

TEST_CASE("Choice log: a session gives the right rows and outcomes", "[app][choicelog]")
{
    Lib lib;
    app::ChoiceTracker t;
    std::int64_t next = 1;
    t.add = [&](const Impression& m) -> std::optional<std::int64_t>
    {
        auto copy = m;
        copy.id = next++;
        std::optional<std::int64_t> id;
        lib.db->writeAndWait([&](library::Connection& c) { id = ChoiceLogRepository(c).add(copy); });
        return id;
    };
    t.setOutcome = [&](std::int64_t id, Outcome o)
    { lib.db->writeAndWait([&](library::Connection& c) { CHECK(ChoiceLogRepository(c).setOutcome(id, o)); }); };

    CHECK_FALSE(t.thumbs(true)); // nothing yet
    // A plays on deck 0. B1 (shown 2nd) on deck 1, heard for 30 s, then replaced by B2 (shown 1st).
    t.loaded(1, impression(10, 21, 2, 5));
    t.played(1, 30.0);
    t.loaded(1, impression(10, 22, 1, 5));
    // B2 played 70 s: mixed; then a thumbs up after the transition.
    t.played(1, 40.0);
    t.played(1, 30.0);
    CHECK(t.thumbs(true));
    // Next: B2 is now A; C (not in the list) on deck 0, loaded but not heard - thumbs go to B2's again.
    t.loaded(0, impression(22, 31, std::nullopt, 5));
    CHECK(t.thumbs(false));
    // Logging off for a load: no row, but nothing pending on deck 1 any more either (B2 was mixed: not ejected).
    t.loaded(1, std::nullopt);

    auto reader = std::move(lib.db->openReader()).value();
    const auto rows = ChoiceLogRepository(*reader).session(77);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].chosenTrack == 21);
    CHECK(rows[0].chosenRank == 2);
    CHECK(rows[0].outcome == Outcome::Ejected);
    CHECK(rows[1].chosenTrack == 22);
    CHECK(rows[1].chosenRank == 1);
    CHECK(rows[1].outcome == Outcome::ThumbsDown); // the last thumbs (down) went to the last one heard
    CHECK(rows[2].chosenTrack == 31);
    CHECK_FALSE(rows[2].chosenRank);
    CHECK(rows[2].outcome == Outcome::Unknown);
    CHECK(rows[2].shown.size() == 6); // the 5 shown and C, scored on the spot
    CHECK(rows[2].shown.back().chosen);
    CHECK(rows[1].intent == Intent::Up);
    CHECK(rows[1].preset == "Balanced");
    CHECK(ChoiceLogRepository(*reader).count() == 3);
}

TEST_CASE("Choice log: CSV and JSON exports, and delete", "[app][choicelog]")
{
    Lib lib;
    lib.db->writeAndWait(
        [&](library::Connection& c)
        {
            ChoiceLogRepository r(c);
            auto a = impression(1, 2, 1, 3);
            a.preset = "Peak, \"loud\""; // needs quoting in CSV and escaping in JSON
            REQUIRE(r.add(a));
            REQUIRE(r.add(impression(2, 3, std::nullopt, 2)));
            REQUIRE(r.setOutcome(1, Outcome::Mixed));
        });
    auto reader = std::move(lib.db->openReader()).value();
    const auto rows = ChoiceLogRepository(*reader).all();
    REQUIRE(rows.size() == 2);
    // Kept where the pandas check (tools/check_choice_export.py) can read them.
    const auto out = std::filesystem::path(DEVILDJ_SOURCE_DIR) / "build" / "choice-export-test";
    std::filesystem::create_directories(out);
    REQUIRE_FALSE(exportCsv(rows, out / "choices.csv"));
    REQUIRE_FALSE(exportJson(rows, out / "choices.json"));
    std::ifstream csv(out / "choices.csv");
    std::string header, line;
    std::getline(csv, header);
    CHECK(header.rfind("impression,session,created_at,context_track,chosen_track,intent,preset", 0) == 0);
    CHECK(header.find(",sound_switch") != std::string::npos);
    int lines = 0;
    while (std::getline(csv, line))
        ++lines;
    CHECK(lines == 3 + 3); // one per shown track: 3, then 2 + the chosen one scored on the spot
    std::stringstream json;
    json << std::ifstream(out / "choices.json").rdbuf();
    CHECK(json.str().find("\"outcome\": \"mixed\"") != std::string::npos);
    CHECK(json.str().find("Peak, \\\"loud\\\"") != std::string::npos);

    lib.db->writeAndWait([&](library::Connection& c) { CHECK(ChoiceLogRepository(c).clear()); });
    CHECK(ChoiceLogRepository(*reader).count() == 0);
}
