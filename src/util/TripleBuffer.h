#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace devildj::util
{

// Lock-free triple buffer: one writer publishes whole values, one reader always gets the latest complete
// value without blocking either side. Used for EngineState (audio thread -> UI).
//
// The writer fills its private back slot (write() or writeBuffer() + publish()); publishing swaps the back
// slot with the shared middle slot. The reader swaps the middle slot into its private front slot only when
// a new value has been published since its last read. Neither side ever waits or allocates.
template <typename T>
class TripleBuffer
{
    static_assert(std::is_copy_assignable_v<T>, "T must be copy assignable");

public:
    TripleBuffer() = default;

    explicit TripleBuffer(const T& initial)
        : slots{initial, initial, initial}
    {
    }

    TripleBuffer(const TripleBuffer&) = delete;
    TripleBuffer& operator=(const TripleBuffer&) = delete;

    // ---- Writer thread ----

    // The writer's private slot. Fill it, then call publish(). Contents after publish() are stale data,
    // so overwrite every field before the next publish.
    T& writeBuffer() noexcept { return slots[back]; }

    void publish() noexcept
    {
        const auto previous = middle.exchange(static_cast<std::uint8_t>(back | kFreshBit), std::memory_order_acq_rel);
        back = static_cast<std::uint8_t>(previous & kIndexMask);
    }

    void write(const T& value) noexcept(std::is_nothrow_copy_assignable_v<T>)
    {
        slots[back] = value;
        publish();
    }

    // ---- Reader thread ----

    // Latest published value (or the initial value if nothing was published yet). The reference stays
    // valid and unchanged until the next call to read() on the reader thread.
    const T& read() noexcept
    {
        if ((middle.load(std::memory_order_relaxed) & kFreshBit) != 0)
        {
            const auto previous = middle.exchange(front, std::memory_order_acq_rel);
            front = static_cast<std::uint8_t>(previous & kIndexMask);
        }
        return slots[front];
    }

    // True if a value was published since the reader's last read().
    bool hasNewData() const noexcept { return (middle.load(std::memory_order_acquire) & kFreshBit) != 0; }

private:
    static constexpr std::uint8_t kIndexMask = 0x3;
    static constexpr std::uint8_t kFreshBit = 0x4;

    std::array<T, 3> slots{};
    std::uint8_t back = 0;               // writer-only
    std::atomic<std::uint8_t> middle{1}; // shared: slot index | fresh bit
    std::uint8_t front = 2;              // reader-only

    static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
};

} // namespace devildj::util
