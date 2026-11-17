#include "util/TripleBuffer.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

using devildj::util::TripleBuffer;

TEST_CASE("TripleBuffer returns the initial value before any publish", "[util][TripleBuffer]")
{
    TripleBuffer<int> tb(42);
    CHECK_FALSE(tb.hasNewData());
    CHECK(tb.read() == 42);
}

TEST_CASE("TripleBuffer reader sees the latest published value", "[util][TripleBuffer]")
{
    TripleBuffer<int> tb(0);
    tb.write(1);
    CHECK(tb.hasNewData());
    CHECK(tb.read() == 1);
    CHECK_FALSE(tb.hasNewData());

    tb.write(2);
    tb.write(3);
    tb.write(4);
    CHECK(tb.read() == 4); // intermediate values are skipped
    CHECK(tb.read() == 4); // no new publish: same value
}

TEST_CASE("TripleBuffer writeBuffer + publish", "[util][TripleBuffer]")
{
    TripleBuffer<std::array<int, 4>> tb;
    auto& slot = tb.writeBuffer();
    slot = {1, 2, 3, 4};
    tb.publish();
    CHECK(tb.read() == std::array<int, 4>{1, 2, 3, 4});
}

namespace
{
// Every field derives from seq, so a torn read (fields from two different publishes) is detectable.
struct Snapshot
{
    std::uint64_t seq = 0;
    std::array<std::uint64_t, 15> payload{};

    void fill(std::uint64_t s)
    {
        seq = s;
        for (std::size_t i = 0; i < payload.size(); ++i)
            payload[i] = s * 31 + i;
    }

    bool consistent() const
    {
        for (std::size_t i = 0; i < payload.size(); ++i)
            if (payload[i] != seq * 31 + i)
                return false;
        return true;
    }
};
} // namespace

TEST_CASE("TripleBuffer two-thread stress: no torn reads, never goes backwards", "[util][TripleBuffer][stress]")
{
    constexpr std::uint64_t kPublishes = 2'000'000;
    Snapshot initial;
    initial.fill(0);
    TripleBuffer<Snapshot> tb(initial);
    std::atomic<bool> done{false};

    std::thread writer(
        [&]
        {
            for (std::uint64_t s = 1; s <= kPublishes; ++s)
            {
                tb.writeBuffer().fill(s);
                tb.publish();
            }
            done.store(true, std::memory_order_release);
        });

    std::uint64_t lastSeq = 0;
    std::uint64_t torn = 0;
    std::uint64_t backwards = 0;
    std::uint64_t reads = 0;
    auto check = [&]
    {
        const auto& s = tb.read();
        ++reads;
        if (!s.consistent())
            ++torn;
        if (s.seq < lastSeq)
            ++backwards;
        lastSeq = s.seq;
    };

    while (!done.load(std::memory_order_acquire))
        check();
    writer.join();
    check();

    CHECK(torn == 0);
    CHECK(backwards == 0);
    CHECK(lastSeq == kPublishes); // after the writer stops, the reader gets the final value
    CHECK(reads > 1);
}
