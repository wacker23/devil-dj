// Smoke tests for src/util.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

TEST_CASE("Index atomics for lock-free queues are always lock-free", "[smoke][util]")
{
    // SpscQueue and TripleBuffer (P0-08) depend on these.
    STATIC_CHECK(std::atomic<std::size_t>::is_always_lock_free);
    STATIC_CHECK(std::atomic<std::uint64_t>::is_always_lock_free);
}

TEST_CASE("Cache-line size is known for padding queue indices", "[smoke][util]")
{
    STATIC_CHECK(std::hardware_destructive_interference_size >= 64);
}
