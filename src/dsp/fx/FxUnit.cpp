#include "dsp/fx/FxUnit.h"

namespace devildj::dsp::fx
{

void FxUnit::prepare(double sampleRate, int maxBlockSize)
{
    for (auto& s : slots)
        s.prepare(sampleRate, maxBlockSize);
}

void FxUnit::reset() noexcept
{
    for (auto& s : slots)
        s.reset();
}

void FxUnit::setMode(Mode m) noexcept
{
    currentMode = m;
    if (m == Mode::Single)
        for (int i = 1; i < kSlots; ++i)
            slots[static_cast<std::size_t>(i)].setOn(false);
}

bool FxUnit::isActive() const noexcept
{
    for (const auto& s : slots)
        if (s.isActive())
            return true;
    return false;
}

void FxUnit::process(float* left, float* right, int numSamples, const BeatContext& beat) noexcept
{
    for (auto& s : slots) // an Off slot doesn't touch the signal, so all off = bit-exact
        s.process(left, right, numSamples, beat);
}

} // namespace devildj::dsp::fx
