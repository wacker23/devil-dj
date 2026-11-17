// P0-04: proves the vcpkg dependencies link into a test target.

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>
#include <xxhash.h>

#include <string_view>

TEST_CASE("SQLite links and runs an in-memory query", "[deps]")
{
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(":memory:", &db) == SQLITE_OK);

    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "SELECT 40 + 2", -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    CHECK(sqlite3_column_int(stmt, 0) == 42);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

TEST_CASE("SQLite has FTS5 for library search", "[deps]")
{
    CHECK(sqlite3_compileoption_used("ENABLE_FTS5") == 1);
}

TEST_CASE("xxHash links and is deterministic", "[deps]")
{
    constexpr std::string_view text = "Devil-DJ";
    const auto a = XXH3_64bits(text.data(), text.size());
    const auto b = XXH3_64bits(text.data(), text.size());
    CHECK(a == b);
    CHECK(a != XXH3_64bits("devil-dj", 8));
}
