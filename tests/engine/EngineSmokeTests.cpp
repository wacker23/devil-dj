// Smoke tests for src/engine.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <type_traits>
#include <variant>

namespace
{
struct FakePlay
{
    std::uint8_t deck;
    bool quantized;
};

struct FakeSeek
{
    std::uint8_t deck;
    double samplePos;
};
} // namespace

TEST_CASE("A variant of POD commands is trivially copyable", "[smoke][engine]")
{
    // EngineCommand relies on this to travel through the lock-free SpscQueue (ARCHITECTURE §3.2).
    using Command = std::variant<FakePlay, FakeSeek>;
    STATIC_CHECK(std::is_trivially_copyable_v<Command>);

    const Command c = FakeSeek{1, 44100.0};
    REQUIRE(std::holds_alternative<FakeSeek>(c));
    CHECK(std::get<FakeSeek>(c).samplePos == 44100.0);
}
