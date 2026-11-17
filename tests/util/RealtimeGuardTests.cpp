#include "util/RealtimeGuard.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <thread>
#include <vector>

using devildj::util::RealtimeViolation;
using devildj::util::ScopedRealtimeContext;

// Note: Catch2's CHECK macros may allocate, so every check here runs outside the real-time scope.

namespace
{
std::atomic<int> allocations{0};
std::atomic<int> deallocations{0};

void countingHandler(RealtimeViolation what) noexcept
{
    if (what == RealtimeViolation::Allocation)
        allocations.fetch_add(1);
    else
        deallocations.fetch_add(1);
}

// Installs the counting handler for one test and restores the previous one afterwards.
struct CountingHandlerScope
{
    CountingHandlerScope()
    {
        allocations = 0;
        deallocations = 0;
        previous = devildj::util::setRealtimeViolationHandler(&countingHandler);
    }
    ~CountingHandlerScope() { devildj::util::setRealtimeViolationHandler(previous); }

    devildj::util::RealtimeViolationHandler previous = nullptr;
};

// Allocation the optimiser cannot remove: goes through the replaceable global operator new.
void* allocateOpaque(std::size_t bytes)
{
    return ::operator new(bytes);
}
} // namespace

#if DEVILDJ_REALTIME_GUARD

TEST_CASE("RealtimeGuard flags new and delete inside a real-time scope", "[util][RealtimeGuard]")
{
    CountingHandlerScope handler;
    const auto before = devildj::util::realtimeViolationCount();
    bool inside = false;
    {
        ScopedRealtimeContext rt;
        inside = devildj::util::isInRealtimeContext();
        void* p = allocateOpaque(64);
        ::operator delete(p);
    }

    CHECK(inside);
    CHECK_FALSE(devildj::util::isInRealtimeContext());
    CHECK(allocations == 1);
    CHECK(deallocations == 1);
    CHECK(devildj::util::realtimeViolationCount() - before == 2);
}

TEST_CASE("RealtimeGuard catches container growth", "[util][RealtimeGuard]")
{
    CountingHandlerScope handler;
    std::vector<int> v;
    {
        ScopedRealtimeContext rt;
        v.push_back(1); // must allocate: the vector starts empty
    }
    CHECK(allocations >= 1);
}

TEST_CASE("RealtimeGuard catches aligned and nothrow allocations", "[util][RealtimeGuard]")
{
    CountingHandlerScope handler;
    {
        ScopedRealtimeContext rt;
        void* a = ::operator new(64, std::align_val_t{64});
        ::operator delete(a, std::align_val_t{64});
        void* b = ::operator new(16, std::nothrow);
        ::operator delete(b, std::nothrow);
    }
    CHECK(allocations == 2);
    CHECK(deallocations == 2);
}

TEST_CASE("RealtimeGuard ignores allocations outside the scope", "[util][RealtimeGuard]")
{
    CountingHandlerScope handler;
    {
        auto owned = std::make_unique<std::vector<int>>(1000, 7);
        owned->push_back(8);
    }
    CHECK(allocations == 0);
    CHECK(deallocations == 0);
}

TEST_CASE("RealtimeGuard scopes nest", "[util][RealtimeGuard]")
{
    CountingHandlerScope handler;
    bool stillInside = false;
    {
        ScopedRealtimeContext outer;
        {
            ScopedRealtimeContext inner;
        }
        stillInside = devildj::util::isInRealtimeContext();
        ::operator delete(allocateOpaque(8));
    }
    CHECK(stillInside);
    CHECK(allocations == 1);
}

TEST_CASE("RealtimeGuard is per thread", "[util][RealtimeGuard]")
{
    CountingHandlerScope handler;
    std::atomic<bool> otherThreadDone{false};
    {
        ScopedRealtimeContext rt;
        // A second thread allocating while this one is real-time must not be flagged.
        // (Creating std::thread itself allocates, so build it before entering the scope in real code.)
    }
    std::thread other(
        [&]
        {
            ::operator delete(allocateOpaque(32));
            otherThreadDone = true;
        });

    {
        ScopedRealtimeContext rt;
        while (!otherThreadDone.load())
            std::this_thread::yield();
    }
    other.join();
    CHECK(allocations == 0);
    CHECK(deallocations == 0);
}

TEST_CASE("RealtimeGuard handler may allocate without recursing", "[util][RealtimeGuard]")
{
    static std::atomic<int> calls{0};
    calls = 0;
    auto previous = devildj::util::setRealtimeViolationHandler(
        [](RealtimeViolation) noexcept
        {
            calls.fetch_add(1);
            auto* scratch = new int(5); // guard is off inside the handler
            delete scratch;
        });
    {
        ScopedRealtimeContext rt;
        ::operator delete(allocateOpaque(8));
    }
    devildj::util::setRealtimeViolationHandler(previous);
    CHECK(calls == 2); // one allocation + one deallocation; the handler's own new/delete aren't counted
}

#else

TEST_CASE("RealtimeGuard is compiled out in Release", "[util][RealtimeGuard]")
{
    STATIC_CHECK_FALSE(devildj::util::kRealtimeGuardEnabled);
    CountingHandlerScope handler;
    bool inside = true;
    {
        ScopedRealtimeContext rt;
        inside = devildj::util::isInRealtimeContext();
        ::operator delete(allocateOpaque(64));
    }
    CHECK_FALSE(inside);
    CHECK(allocations == 0);
    CHECK(devildj::util::realtimeViolationCount() == 0);
    STATIC_CHECK(sizeof(ScopedRealtimeContext) == 1); // empty object, no state
}

#endif

#if DEVILDJ_REALTIME_GUARD
// Hidden (not run by ctest): shows the default handler's one-time stack trace on stderr.
// Run: DevilDJTests.exe "[.realtime-demo]"
TEST_CASE("RealtimeGuard default handler demo", "[.realtime-demo]")
{
    {
        ScopedRealtimeContext rt;
        ::operator delete(allocateOpaque(8));
    }
    CHECK(devildj::util::realtimeViolationCount() >= 2);
}
#endif
