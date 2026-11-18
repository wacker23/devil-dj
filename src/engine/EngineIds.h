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

// Mixer channels: one per deck for now (sampler / Beat Maker / Code Beat / mic channels come later).
constexpr int kMaxChannels = kMaxDecks;

// Per-channel continuous parameters. Units: Trim and EQ bands in dB (EQ <= -100 = kill), Filter 0..1 (0.5 = off),
// FilterResonance 0..1, Fader 0..1, EqMode 0 = EQ / 1 = isolator, FaderCurve 0 smooth / 1 linear / 2 sharp.
enum class ChannelParam : std::uint16_t
{
    Trim,
    EqLow,
    EqMid,
    EqHigh,
    Filter,
    FilterResonance,
    Fader,
    EqMode,
    FaderCurve,
    Count
};
constexpr int kChannelParamCount = static_cast<int>(ChannelParam::Count);

// Flat id of every continuous parameter (ARCHITECTURE 3.2) so MIDI Learn and the keymap address them the same
// way: channel 0 Trim, EqLow, ... then channel 1 ... Later tasks append (crossfader, master, FX, ...).
enum class ParamId : std::uint16_t
{
    ChannelFirst = 0,
    ChannelLast = kMaxChannels * kChannelParamCount - 1,
    Count
};

constexpr ParamId channelParam(int channel, ChannelParam p) noexcept
{
    return static_cast<ParamId>(channel * kChannelParamCount + static_cast<int>(p));
}

constexpr bool isChannelParam(ParamId id) noexcept
{
    return id <= ParamId::ChannelLast;
}

} // namespace devildj::engine
