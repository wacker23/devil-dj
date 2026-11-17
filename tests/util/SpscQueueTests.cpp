#include "util/SpscQueue.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <thread>

using devildj::util::SpscQueue;

TEST_CASE("SpscQueue starts empty", "[util][SpscQueue]")
{
    SpscQueue<int, 8> q;
    int out = -1;
    CHECK_FALSE(q.tryPop(out));
    CHECK(out == -1);
    CHECK(q.emptyApprox());
    CHECK_FALSE(q.tryPop().has_value());
}

TEST_CASE("SpscQueue is FIFO and holds exactly Capacity items", "[util][SpscQueue]")
{
    SpscQueue<int, 8> q;
    for (int i = 0; i < 8; ++i)
        REQUIRE(q.tryPush(i));

    CHECK(q.sizeApprox() == 8);
    CHECK_FALSE(q.tryPush(99)); // full

    for (int i = 0; i < 8; ++i)
    {
        int out = -1;
        REQUIRE(q.tryPop(out));
        CHECK(out == i);
    }
    CHECK(q.emptyApprox());
}

TEST_CASE("SpscQueue wraps around many times", "[util][SpscQueue]")
{
    SpscQueue<std::uint32_t, 4> q;
    std::uint32_t next = 0;
    std::uint32_t expected = 0;
    for (int round = 0; round < 1000; ++round)
    {
        // Push 3, pop 3: the indices keep moving through the ring.
        for (int i = 0; i < 3; ++i)
            REQUIRE(q.tryPush(next++));
        for (int i = 0; i < 3; ++i)
        {
            const auto v = q.tryPop();
            REQUIRE(v.has_value());
            CHECK(*v == expected++);
        }
    }
}

TEST_CASE("SpscQueue supports move-only types", "[util][SpscQueue]")
{
    SpscQueue<std::unique_ptr<int>, 2> q;
    REQUIRE(q.tryPush(std::make_unique<int>(7)));
    auto out = q.tryPop();
    REQUIRE(out.has_value());
    REQUIRE(*out != nullptr);
    CHECK(**out == 7);
}

TEST_CASE("SpscQueue full push leaves the value untouched", "[util][SpscQueue]")
{
    SpscQueue<std::unique_ptr<int>, 2> q;
    REQUIRE(q.tryPush(std::make_unique<int>(1)));
    REQUIRE(q.tryPush(std::make_unique<int>(2)));
    auto third = std::make_unique<int>(3);
    CHECK_FALSE(q.tryPush(std::move(third)));
    REQUIRE(third != nullptr); // not moved from
    CHECK(*third == 3);
}

TEST_CASE("SpscQueue two-thread stress: 10M items, no loss, no reorder", "[util][SpscQueue][stress]")
{
    constexpr std::uint64_t kItems = 10'000'000;
    SpscQueue<std::uint64_t, 1024> q;

    std::thread producer(
        [&q]
        {
            for (std::uint64_t i = 0; i < kItems; ++i)
                while (!q.tryPush(i))
                    std::this_thread::yield();
        });

    std::uint64_t expected = 0;
    std::uint64_t outOfOrder = 0;
    while (expected < kItems)
    {
        std::uint64_t v = 0;
        if (q.tryPop(v))
        {
            if (v != expected)
                ++outOfOrder;
            ++expected;
        }
        else
        {
            std::this_thread::yield();
        }
    }
    producer.join();

    CHECK(outOfOrder == 0);
    CHECK(expected == kItems);
    CHECK(q.emptyApprox());
}
