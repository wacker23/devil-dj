#pragma once

#include "dsp/fx/FxSlot.h"

#include <array>

namespace devildj::dsp::fx
{

// An FX unit (P3-08): three slots in series (multi mode, each effect with its one knob) or one effect with all
// its parameters (single mode: slot 0; slots 1 and 2 are switched off, letting their tails ring out). Where its
// output goes (a deck, the sampler, the code deck, the master) is the engine's business. Audio thread.
class FxUnit
{
public:
    static constexpr int kSlots = 3;
    enum class Mode
    {
        Multi,
        Single,
    };

    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;

    FxSlot& slot(int index) noexcept { return slots[static_cast<std::size_t>(index)]; }
    const FxSlot& slot(int index) const noexcept { return slots[static_cast<std::size_t>(index)]; }

    void setMode(Mode m) noexcept;
    Mode mode() const noexcept { return currentMode; }
    // A slot may be switched on in this mode (single: slot 0 only).
    bool slotUsable(int index) const noexcept { return currentMode == Mode::Multi || index == 0; }

    bool isActive() const noexcept; // any slot sounding (on, fading or ringing out)
    void process(float* left, float* right, int numSamples, const BeatContext& beat) noexcept;

private:
    std::array<FxSlot, kSlots> slots;
    Mode currentMode = Mode::Multi;
};

} // namespace devildj::dsp::fx
