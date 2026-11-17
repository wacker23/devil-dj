#pragma once

#include "util/SpscQueue.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace devildj::util
{

// Defers destruction of objects from the audio thread to a non-realtime thread.
//
// The audio thread hands over pointers with tryRelease() (no allocation, no locking, no destructor runs);
// a housekeeping thread (the engine's 10 Hz timer) calls collect(), which runs the deleters. One releasing
// thread and one collecting thread, like SpscQueue.
//
// If the queue is full, tryRelease() returns false and the caller keeps ownership; it should retry on a
// later block. failedReleases() counts such cases so they show up in diagnostics.
template <std::size_t Capacity = 1024>
class GarbageQueue
{
public:
    using Deleter = void (*)(void*) noexcept;

    GarbageQueue() = default;
    GarbageQueue(const GarbageQueue&) = delete;
    GarbageQueue& operator=(const GarbageQueue&) = delete;

    // Frees anything still queued. Destroy only when neither side is running.
    ~GarbageQueue() { collect(); }

    // ---- Releasing (audio) thread ----

    // Queues 'object' to be destroyed with 'delete' on the collecting thread. Null is accepted and ignored.
    template <typename T>
    bool tryRelease(T* object) noexcept
    {
        if (object == nullptr)
            return true;
        return tryReleaseWith(object, &deleteAs<T>);
    }

    // Queues an arbitrary cleanup call, e.g. dropping a registry entry by id. 'context' is passed to 'fn'.
    bool tryReleaseWith(void* context, Deleter fn) noexcept
    {
        if (queue.tryPush(Entry{fn, context}))
            return true;

        failed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // ---- Collecting (non-realtime) thread ----

    // Runs all queued deleters. Returns how many objects were freed.
    std::size_t collect() noexcept
    {
        std::size_t count = 0;
        Entry entry;
        while (queue.tryPop(entry))
        {
            entry.fn(entry.context);
            ++count;
        }
        return count;
    }

    std::size_t pendingApprox() const noexcept { return queue.sizeApprox(); }
    std::uint64_t failedReleases() const noexcept { return failed.load(std::memory_order_relaxed); }

private:
    struct Entry
    {
        Deleter fn = nullptr;
        void* context = nullptr;
    };

    template <typename T>
    static void deleteAs(void* p) noexcept
    {
        delete static_cast<T*>(p);
    }

    SpscQueue<Entry, Capacity> queue;
    std::atomic<std::uint64_t> failed{0};
};

} // namespace devildj::util
