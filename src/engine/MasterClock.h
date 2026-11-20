#pragma once

#include <cstdint>
#include <span>

namespace devildj::engine
{

// What the clock needs from each deck at the start of a block (the playhead after the last block).
struct ClockDeckInput
{
    bool playing = false;
    bool hasGrid = false;
    bool synced = false; // SYNC on (P2-19): preferred as the leader in Auto
    double beat = 0.0;  // the deck's grid beat at its playhead
    double bpm = 0.0;   // its tempo as heard: the grid's BPM there x the deck's rate
};

// The master clock (P2-18; ARCHITECTURE §3.5): master BPM and beat position (double beats), advanced once per
// block on the audio thread, for sync (P2-19), quantize (P2-20) and the scheduler (Beat Maker, Code Beat).
//
// Source: a chosen deck, Auto (the leading deck while it plays, else the first playing synced deck with a grid,
// else the first playing deck with a grid) or the internal tempo. While a deck
// leads, the clock's beat is that deck's grid beat plus a whole number of bars (chosen when the deck takes over,
// as close as possible to where the clock was), so bar counts don't jump and the beat follows the deck's phase.
// With no deck leading, the clock runs on from where it was at the internal tempo - which takes the last
// leading deck's tempo - so the beat never jumps. Within a block the beat is linear in samples.
//
// Real-time safe: no allocation, no locks. Audio thread only, apart from prepare / reset.
class MasterClock
{
public:
    enum class Mode : std::uint8_t
    {
        Auto,
        Deck,
        Internal,
    };
    static constexpr int kInternal = -1;
    static constexpr double kMinBpm = 20.0, kMaxBpm = 300.0;
    static constexpr int kBeatsPerBar = 4;

    void prepare(double sampleRate) noexcept;
    void reset() noexcept; // sample 0, beat 0, internal 120 BPM, Auto

    void setMode(Mode mode, int deck = kInternal) noexcept;
    void setInternalBpm(double bpm) noexcept; // clamped to 20..300
    Mode mode() const noexcept { return currentMode; }
    double internalBpm() const noexcept { return internal; }

    // Advances to the next block: [blockStart(), blockStart() + numSamples).
    void beginBlock(int numSamples, std::span<const ClockDeckInput> decks) noexcept;

    std::int64_t blockStart() const noexcept { return start; }
    std::int64_t blockEnd() const noexcept { return start + length; }
    double bpm() const noexcept { return tempo; }
    double beatAtBlockStart() const noexcept { return beatStart; }
    double beatAtBlockEnd() const noexcept { return beatAt(static_cast<double>(blockEnd())); }
    int sourceDeck() const noexcept { return source; } // kInternal when the internal tempo leads
    double beatsPerSample() const noexcept { return tempo / (60.0 * rate); }

    // Beat <-> engine sample clock, at the current block's tempo (producers schedule at most ~150 ms ahead, so a
    // tempo change on the way moves an event by a negligible amount).
    double beatAt(double sampleTime) const noexcept;
    double sampleAt(double beat) const noexcept;

private:
    int chooseSource(std::span<const ClockDeckInput> decks) const noexcept;

    double rate = 48000.0;
    Mode currentMode = Mode::Auto;
    int chosenDeck = kInternal;
    double internal = 120.0;

    std::int64_t start = 0;
    int length = 0;
    double tempo = 120.0;
    double beatStart = 0.0;
    int source = kInternal;
    double barOffset = 0.0; // beats added to the leading deck's beat (a multiple of 4)
};

} // namespace devildj::engine
