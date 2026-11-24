#pragma once

// Drum Swap banks (P6-05): 8 slots per deck (A and B), each a loop of whole beats that can play in place of the
// deck's drums stem. Kept across runs in Documents\Devil-DJ\DrumSwap\Deck A|B\: slot-1.wav ... slot-8.wav (24-bit
// stereo) and bank.json (names, beats, single / multi). Loaded at the device rate (resampled when a file's
// differs). Message thread.

#include "engine/StemBuffer.h"
#include "engine/TrackBuffer.h"
#include "util/Beatgrid.h"
#include "util/Result.h"

#include <juce_core/juce_core.h>

#include <array>
#include <memory>

namespace devildj::app
{

struct DrumSwapSlot
{
    juce::String name; // "CAP 8", "BEAT A", ...
    double beats = 0.0;
    std::shared_ptr<const engine::TrackBuffer> audio; // nullptr: empty
    bool empty() const noexcept { return audio == nullptr; }
};

class DrumSwapBank
{
public:
    static constexpr int kDecks = 2;
    static constexpr int kSlots = 8;
    static constexpr int kCaptureBeats[] = {4, 8, 16, 32};

    explicit DrumSwapBank(juce::File folder);
    static juce::File defaultFolder(); // Documents\Devil-DJ\DrumSwap

    // Reads both banks; slots whose files are missing or unreadable stay empty. Returns how many were loaded.
    int load(double sampleRate);

    const DrumSwapSlot& slot(int deck, int index) const;
    // Puts a loop in a slot and saves it (the WAV and bank.json). Returns an error, or empty.
    juce::String set(int deck, int index, DrumSwapSlot slot);
    juce::String clear(int deck, int index);

    bool multi(int deck) const noexcept { return decks[static_cast<std::size_t>(clampDeck(deck))].multi; }
    void setMulti(int deck, bool on);
    int captureBeats() const noexcept { return capture; } // the default length for Shift + pad
    void setCaptureBeats(int beats);

    juce::File deckFolder(int deck) const;

private:
    struct Deck
    {
        std::array<DrumSwapSlot, kSlots> slots;
        bool multi = false;
    };
    static int clampDeck(int deck) noexcept { return deck < 0 ? 0 : deck >= kDecks ? kDecks - 1 : deck; }
    juce::String saveJson(int deck) const;

    juce::File folder;
    std::array<Deck, kDecks> decks;
    int capture = 8;
};

// The drums stem from the bar at or before `position` (the deck's beatgrid, buffer samples), `beats` beats long,
// as a loop of its own (float stereo at the stems' rate). Fails without a grid or past the track's end.
util::Result<std::shared_ptr<engine::TrackBuffer>> captureDrums(const engine::StemBuffer& stems,
                                                                const util::Beatgrid& grid, double position,
                                                                int beats);

} // namespace devildj::app
