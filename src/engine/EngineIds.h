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

// Mixer channels: one per deck, then the sampler (P3-13), the Beat Maker "BEATS" (P4-10), the code deck "CODE"
// (P5-12); a mic later.
constexpr int kSamplerChannel = kMaxDecks;
constexpr int kBeatMakerChannel = kMaxDecks + 1;
constexpr int kCodeDeckChannel = kMaxDecks + 2;
constexpr int kMaxChannels = kMaxDecks + 3;

// Per-channel continuous parameters. Units: Trim and EQ bands in dB (EQ <= -100 = kill), Filter 0..1 (0.5 = off),
// FilterResonance 0..1, Fader 0..1, EqMode 0 = EQ / 1 = isolator, FaderCurve 0 smooth / 1 linear / 2 sharp,
// CrossfaderAssign 0 A / 1 THRU / 2 B, Cue 0 / 1.
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
    CrossfaderAssign, // 0 A, 1 THRU, 2 B
    Cue,              // headphone CUE button: 0 off, 1 on
    Count
};
constexpr int kChannelParamCount = static_cast<int>(ChannelParam::Count);

// Flat id of every continuous parameter (ARCHITECTURE 3.2) so MIDI Learn and the keymap address them the same
// way: channel 0 Trim, EqLow, ... then channel 1 ... Later tasks append (crossfader, master, FX, ...).
// FX units (P3-08): two, each with 3 slots; where each one is inserted.
constexpr int kFxUnits = 2;
constexpr int kFxSlots = 3;
enum class FxTarget : std::uint8_t
{
    Deck0, // decks 1-4: in the channel strip, after the filter / color FX, before the fader
    Deck1,
    Deck2,
    Deck3,
    Sampler,   // the sampler channel (P3-13)
    BeatMaker, // the Beat Maker channel (P4-10)
    CodeDeck,  // the code deck channel (P5-12)
    Master,    // the master bus, before master gain and the limiter
};
constexpr int kFxTargetCount = 8;
// Pad FX (P3-12): an effect slot per pad, per channel (the pads' current bank).
constexpr int kPadFxSlots = 8;

// Hot cues per deck (P3-01): two pages of 8 pads (A-H, I-P).
constexpr int kHotCues = 16;

enum class ParamId : std::uint16_t
{
    ChannelFirst = 0,
    ChannelLast = kMaxChannels * kChannelParamCount - 1,
    Crossfader,      // 0 (A side) .. 1 (B side)
    CrossfaderCurve, // 0 smooth, 1 linear, 2 cut
    MasterGain,      // dB, <= -100 = silent, max +6
    CueMix,          // headphones: 0 = cue only, 0.5 = both at full level, 1 = master only
    HeadphoneLevel,  // dB, <= -100 = silent, max +6
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
