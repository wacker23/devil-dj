#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

namespace devildj::util
{

// Cache-line size used to keep producer and consumer indices apart (avoids false sharing).
inline constexpr std::size_t kCacheLineSize = 64;

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // structure was padded due to alignment specifier (intended)
#endif

// Bounded single-producer / single-consumer queue. Wait-free and allocation-free after construction,
// so it is safe on the audio thread from either end.
//
// - Exactly one thread may call tryPush(), and exactly one (other) thread may call tryPop().
// - Capacity must be a power of two; all Capacity slots are usable.
// - The queue stores T by value in a fixed array. Elements are moved in and out; for audio-thread use,
//   T should be trivially copyable (e.g. EngineCommand) so no destructor runs on the audio thread.
template <typename T, std::size_t Capacity>
class SpscQueue
{
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two >= 2");
    static_assert(std::is_default_constructible_v<T>, "T must be default constructible");
    static_assert(std::is_nothrow_move_assignable_v<T>, "T must be nothrow move assignable");

public:
    using value_type = T;
    static constexpr std::size_t capacity() noexcept { return Capacity; }

    SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Producer side. Returns false (and leaves value untouched) if the queue is full.
    bool tryPush(const T& value) noexcept(std::is_nothrow_copy_assignable_v<T>)
    {
        const auto tail = writeIndex.load(std::memory_order_relaxed);
        if (!hasSpace(tail))
            return false;

        slots[tail & kMask] = value;
        writeIndex.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool tryPush(T&& value) noexcept
    {
        const auto tail = writeIndex.load(std::memory_order_relaxed);
        if (!hasSpace(tail))
            return false;

        slots[tail & kMask] = std::move(value);
        writeIndex.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false if the queue is empty.
    bool tryPop(T& out) noexcept
    {
        const auto head = readIndex.load(std::memory_order_relaxed);
        if (head == cachedWriteIndex)
        {
            cachedWriteIndex = writeIndex.load(std::memory_order_acquire);
            if (head == cachedWriteIndex)
                return false;
        }

        out = std::move(slots[head & kMask]);
        readIndex.store(head + 1, std::memory_order_release);
        return true;
    }

    std::optional<T> tryPop() noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        T value{};
        if (!tryPop(value))
            return std::nullopt;
        return value;
    }

    // Approximate when called while the other side is active; exact when both sides are idle.
    std::size_t sizeApprox() const noexcept
    {
        const auto tail = writeIndex.load(std::memory_order_acquire);
        const auto head = readIndex.load(std::memory_order_acquire);
        return tail - head;
    }

    bool emptyApprox() const noexcept { return sizeApprox() == 0; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    // Producer-only: is there room at 'tail'? Refreshes the cached read index only when it looks full.
    bool hasSpace(std::size_t tail) noexcept
    {
        if (tail - cachedReadIndex < Capacity)
            return true;

        cachedReadIndex = readIndex.load(std::memory_order_acquire);
        return tail - cachedReadIndex < Capacity;
    }

    // Monotonic counters (wrap at 2^64, never in practice); slot = counter & kMask.
    alignas(kCacheLineSize) std::atomic<std::size_t> writeIndex{0}; // written by producer
    alignas(kCacheLineSize) std::size_t cachedReadIndex = 0;        // producer's copy of readIndex
    alignas(kCacheLineSize) std::atomic<std::size_t> readIndex{0};  // written by consumer
    alignas(kCacheLineSize) std::size_t cachedWriteIndex = 0;       // consumer's copy of writeIndex
    alignas(kCacheLineSize) std::array<T, Capacity> slots{};

    static_assert(std::atomic<std::size_t>::is_always_lock_free);
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace devildj::util
