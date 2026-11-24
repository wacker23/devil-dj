#pragma once

// Runs `work` on a thread with a 16 MB stack and waits for it: QuickJS in Debug builds needs far more C stack per
// JS call than Catch2's 1 MB main thread has. Create the JS runtime inside `work` (it is thread-confined), gather
// results there and check them afterwards on the main thread (Catch2 assertions stay on one thread).

#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include <functional>

namespace devildj::testing
{
inline void onBigStack(std::function<void()> work)
{
    struct Worker final : juce::Thread
    {
        std::function<void()> fn;
        explicit Worker(std::function<void()> f)
            : juce::Thread("js test", 16 * 1024 * 1024)
            , fn(std::move(f))
        {
        }
        void run() override { fn(); }
    } worker(std::move(work));
    worker.startThread();
    REQUIRE(worker.waitForThreadToExit(120000));
}
} // namespace devildj::testing
