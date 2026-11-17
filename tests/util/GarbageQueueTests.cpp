#include "util/GarbageQueue.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

using devildj::util::GarbageQueue;

namespace
{
std::atomic<int> liveObjects{0};

struct Tracked
{
    Tracked() { liveObjects.fetch_add(1); }
    ~Tracked() { liveObjects.fetch_sub(1); }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
};
} // namespace

TEST_CASE("GarbageQueue frees objects on collect, not on release", "[util][GarbageQueue]")
{
    liveObjects = 0;
    GarbageQueue<8> gq;

    REQUIRE(gq.tryRelease(new Tracked));
    REQUIRE(gq.tryRelease(new Tracked));
    CHECK(liveObjects == 2); // still alive: nothing freed on the releasing thread
    CHECK(gq.pendingApprox() == 2);

    CHECK(gq.collect() == 2);
    CHECK(liveObjects == 0);
    CHECK(gq.collect() == 0);
}

TEST_CASE("GarbageQueue accepts null", "[util][GarbageQueue]")
{
    GarbageQueue<4> gq;
    CHECK(gq.tryRelease<Tracked>(nullptr));
    CHECK(gq.pendingApprox() == 0);
}

TEST_CASE("GarbageQueue reports a full queue and the caller keeps ownership", "[util][GarbageQueue]")
{
    liveObjects = 0;
    GarbageQueue<2> gq;
    REQUIRE(gq.tryRelease(new Tracked));
    REQUIRE(gq.tryRelease(new Tracked));

    auto* extra = new Tracked;
    CHECK_FALSE(gq.tryRelease(extra));
    CHECK(gq.failedReleases() == 1);

    gq.collect();
    CHECK(liveObjects == 1); // 'extra' is still ours
    REQUIRE(gq.tryRelease(extra));
    gq.collect();
    CHECK(liveObjects == 0);
}

TEST_CASE("GarbageQueue custom cleanup function", "[util][GarbageQueue]")
{
    static int cleaned = 0;
    cleaned = 0;
    int registryId = 17;

    GarbageQueue<4> gq;
    REQUIRE(gq.tryReleaseWith(&registryId, [](void* p) noexcept { cleaned = *static_cast<int*>(p); }));
    gq.collect();
    CHECK(cleaned == 17);
}

TEST_CASE("GarbageQueue destructor frees what is left", "[util][GarbageQueue]")
{
    liveObjects = 0;
    {
        GarbageQueue<4> gq;
        REQUIRE(gq.tryRelease(new Tracked));
    }
    CHECK(liveObjects == 0);
}

TEST_CASE("GarbageQueue two-thread stress: releaser and collector", "[util][GarbageQueue][stress]")
{
    liveObjects = 0;
    constexpr int kObjects = 200'000;
    GarbageQueue<256> gq;
    std::atomic<bool> done{false};

    std::thread collector(
        [&]
        {
            while (!done.load(std::memory_order_acquire))
            {
                gq.collect();
                std::this_thread::yield();
            }
            gq.collect();
        });

    for (int i = 0; i < kObjects; ++i)
    {
        auto* obj = new Tracked;
        while (!gq.tryRelease(obj))
            std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
    collector.join();

    CHECK(liveObjects == 0);
}
