#pragma once

#include <array>
#include <cstdint>

namespace devildj::engine
{

class Sampler;

// The sampler's sequencer (P3-15): sampler pad presses (and gate releases) recorded into a 1 / 2 / 4-bar loop on
// the master clock's bars, quantized to 1/16 beat, played back sample-accurately; overdub (record while it
// plays), mute per slot, erase (all / a slot). Audio thread, no allocation: the events live in a fixed array.
//
// A sequence's beat b sounds wherever the master clock is at b (mod its length) - it is pinned to the master
// clock's bars, so it is in time with the decks and replays the same however it is started.
struct SequenceEvent
{
    float beat = 0.0f;      // 0 .. length (beats), on the 1/16 grid
    std::uint8_t slot = 0;  // sampler slot 0..31
    std::uint8_t kind = 0;  // 0 press, 1 release (a gate let go)
};

constexpr int kMaxSequenceEvents = 128;

struct Sequence
{
    std::uint8_t bars = 1; // 1, 2 or 4
    int count = 0;
    std::array<SequenceEvent, kMaxSequenceEvents> events{};
    double lengthBeats() const noexcept { return 4.0 * bars; }
};

class Sequencer
{
public:
    static constexpr double kGrid = 0.25; // 1/16 note = 1/4 beat

    void reset() noexcept;
    const Sequence& sequence() const noexcept { return seq; }

    // Loading (from the app): clear with a length, then the events one by one.
    void clear(int bars) noexcept;
    bool add(const SequenceEvent& event) noexcept; // false when full
    void setBars(int bars) noexcept;               // keeps the events that still fit

    void setPlaying(bool on) noexcept;
    void setRecording(bool on) noexcept; // recording also plays (overdub)
    bool playing() const noexcept { return isPlaying; }
    bool recording() const noexcept { return isRecording; }
    void setMuted(int slot, bool muted) noexcept;
    std::uint32_t mutedSlots() const noexcept { return mutes; }
    void erase() noexcept;            // every event
    void eraseSlot(int slot) noexcept; // one slot's events

    // A pad press / release at master beat `beat` while recording: the event goes in at the nearest 1/16.
    void record(int slot, bool release, double beat) noexcept;

    // The block [beatStart, beatEnd) of the master clock: presses / releases due in it go to the sampler at their
    // sample offsets (samplesPerBeat: of this block's tempo).
    void process(Sampler& sampler, double beatStart, double beatEnd, double samplesPerBeat, int numSamples) noexcept;
    // Where in the sequence the master clock is (0 .. length), for the display.
    double positionAt(double beat) const noexcept;

private:
    Sequence seq;
    // A just-recorded event isn't played again in the pass it was recorded in (it was heard live).
    std::array<std::int64_t, kMaxSequenceEvents> recordedInPass{};
    bool isPlaying = false, isRecording = false;
    std::uint32_t mutes = 0;
};

} // namespace devildj::engine
