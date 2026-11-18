#include "util/ThreadPriority.h"

#include <catch2/catch_test_macros.hpp>

#include <thread>

using namespace devildj::util;

TEST_CASE("Thread priority: set, read back, and what a new thread starts with", "[util][priority]")
{
    int seen = 0, inherited = 0, startedWith = 0;
    std::thread(
        [&]
        {
            setCurrentThreadPriority(backgroundThreadPriority());
            seen = currentThreadPriority();
            // A thread started from here does NOT inherit it (Windows starts it at normal) ...
            const int mine = currentThreadPriority();
            std::thread([&] { startedWith = currentThreadPriority(); }).join();
            // ... which is why workers copy their creator's priority explicitly.
            std::thread(
                [&, mine]
                {
                    setCurrentThreadPriority(mine);
                    inherited = currentThreadPriority();
                })
                .join();
        })
        .join();
    CHECK(seen == backgroundThreadPriority());
    CHECK(inherited == backgroundThreadPriority());
#if defined(_WIN32)
    CHECK(startedWith != backgroundThreadPriority());
#endif
    CHECK(audioThreadPriority() > backgroundThreadPriority());
}
