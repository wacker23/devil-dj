#pragma once

#include <cstdint>

namespace devildj::engine
{

using DeckId = std::uint8_t;    // 0..3 (A, B, C, D)
using BufferId = std::uint32_t; // BufferRegistry id; 0 = none

constexpr int kMaxDecks = 4;

constexpr char deckLetter(DeckId deck) noexcept
{
    return static_cast<char>('A' + deck);
}

} // namespace devildj::engine
