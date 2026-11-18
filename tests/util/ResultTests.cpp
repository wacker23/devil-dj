#include "util/Result.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using devildj::util::Result;

TEST_CASE("Result holds a value or an error", "[util][Result]")
{
    auto good = Result<int>::ok(42);
    CHECK(good.isOk());
    CHECK(static_cast<bool>(good));
    CHECK(good.value() == 42);
    CHECK(good.error().empty());

    auto bad = Result<int>::fail("disk full");
    CHECK_FALSE(bad.isOk());
    CHECK(bad.error() == "disk full");
}

TEST_CASE("Result works with move-only values", "[util][Result]")
{
    auto r = Result<std::unique_ptr<int>>::ok(std::make_unique<int>(7));
    REQUIRE(r);
    auto owned = std::move(r).value();
    REQUIRE(owned != nullptr);
    CHECK(*owned == 7);
}
