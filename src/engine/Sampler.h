#pragma once

#include "dsp/Stretcher.h"
#include "engine/EngineIds.h"
#include "engine/TrackBuffer.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace devildj::engine
{

// The sampler (P3-13): 4 banks x 8 slots of samples, up to 8 playing at once (voices). Audio thread; prepare
// allocates (a stretcher per voice).
//
// Modes: one-shot (a press plays it to its end; a press while it plays starts it again), loop (a press starts it
// looping, the next press stops it), gate (plays while the pad is held). A start is quantized when asked (the
// engine works out how many samples to wait, from the master clock). Sync: the sample is time-stretched to the
// master tempo (its own BPM from the library, or worked out as a whole number of bars), so synced loops started
// on the beat stay on it; the stretcher's latency is primed away, so what is heard starts on the sample's first
// frame at the start time. Per-slot gain. Voices fade in / out over 2 ms (starts, stops, a stolen voice).
class Sampler
{
public:
    static constexpr int kBanks = 4;
    static constexpr int kSlotsPerBank = 8;
    static constexpr int kSlots = kBanks * kSlotsPerBank;
    static constexpr int kVoices = 8;

    enum class Mode : std::uint8_t
    {
        OneShot,
        Loop,
        Gate,
    };

    struct Slot
    {
        const TrackBuffer* buffer = nullptr;
        BufferId id = 0;
        Mode mode = Mode::OneShot;
        float gain = 1.0f;
        bool sync = false;
        double bpm = 0.0; // the sample's own tempo; 0 = unknown (no sync)
    };

    Sampler();
    ~Sampler();
    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept; // all voices stop at once

    // A new sample in a slot (nullptr = empty). Voices playing the old one stop at once; returns its id (0 if
    // none) for the caller to release.
    BufferId load(int slot, const TrackBuffer* buffer, BufferId id, double bpm) noexcept;
    void setMode(int slot, Mode mode) noexcept;
    void setGain(int slot, float gain) noexcept;
    void setSync(int slot, bool on) noexcept;
    const Slot& slot(int index) const noexcept { return slots[static_cast<std::size_t>(index)]; }

    // The pad: down / up. delaySamples: when to start (quantized), from the next render's first sample.
    void press(int slot, int delaySamples) noexcept;
    void releasePad(int slot) noexcept;
    void stop(int slot) noexcept;
    void stopAll() noexcept;

    // A loop's tempo when nothing better is known: its length as a whole number of beats - the power of two
    // nearest to what it would be at `masterBpm` - over its duration. 0 if that isn't between 60 and 200 BPM.
    static double estimateLoopBpm(double seconds, double masterBpm) noexcept;

    bool isPlaying(int slot) const noexcept;
    double progress(int slot) const noexcept; // 0..1 of the playing voice, -1 = not playing

    // Adds every voice into left / right (which the caller cleared). masterBpm: for sync.
    void render(float* left, float* right, int numSamples, double masterBpm) noexcept;

private:
    struct Voice
    {
        bool active = false;
        int slot = -1;
        const TrackBuffer* buffer = nullptr;
        int delay = 0;          // samples to wait before it starts
        double position = 0.0;  // heard position in the sample (samples)
        double rate = 1.0;      // input samples per output sample
        bool stretched = false; // through the stretcher (sync at a rate other than 1)
        std::int64_t feed = 0;  // next input sample for the stretcher
        bool primed = false;
        int fadeIn = 0, fadeOut = -1; // -1 = not fading out
        std::uint64_t started = 0;    // for stealing the oldest
        std::unique_ptr<dsp::Stretcher> stretcher;
    };

    Voice* voiceFor(int slot) noexcept;
    Voice& freeVoice() noexcept;
    void startVoice(Voice& v, int slot, int delay) noexcept;
    void renderVoice(Voice& v, float* left, float* right, int numSamples, double masterBpm) noexcept;
    float frame(const Voice& v, int channel, std::int64_t index) const noexcept;

    double sampleRate = 48000.0;
    int fadeSamples = 96;
    int maxBlock = 512;
    std::array<Slot, kSlots> slots{};
    std::array<Voice, kVoices> voices;
    std::uint64_t counter = 0;
    std::vector<float> inL, inR, outL, outR, seekL, seekR;
};

} // namespace devildj::engine
