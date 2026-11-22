#include "engine/Sequencer.h"

#include "engine/Sampler.h"

#include <algorithm>
#include <cmath>

namespace devildj::engine
{

namespace
{
int clampBars(int bars) noexcept
{
    return bars >= 4 ? 4 : bars >= 2 ? 2 : 1;
}
} // namespace

void Sequencer::reset() noexcept
{
    clear(1);
    isPlaying = isRecording = false;
    mutes = 0;
}

void Sequencer::clear(int bars) noexcept
{
    seq.bars = static_cast<std::uint8_t>(clampBars(bars));
    seq.count = 0;
    recordedInPass.fill(-1);
}

bool Sequencer::add(const SequenceEvent& event) noexcept
{
    if (seq.count >= kMaxSequenceEvents || event.slot >= Sampler::kSlots)
        return false;
    if (!(event.beat >= 0.0f) || event.beat >= static_cast<float>(seq.lengthBeats()))
        return false;
    seq.events[static_cast<std::size_t>(seq.count)] = event;
    recordedInPass[static_cast<std::size_t>(seq.count)] = -1;
    ++seq.count;
    return true;
}

void Sequencer::setBars(int bars) noexcept
{
    seq.bars = static_cast<std::uint8_t>(clampBars(bars));
    int kept = 0;
    for (int i = 0; i < seq.count; ++i)
        if (seq.events[static_cast<std::size_t>(i)].beat < static_cast<float>(seq.lengthBeats()))
        {
            seq.events[static_cast<std::size_t>(kept)] = seq.events[static_cast<std::size_t>(i)];
            recordedInPass[static_cast<std::size_t>(kept)] = recordedInPass[static_cast<std::size_t>(i)];
            ++kept;
        }
    seq.count = kept;
}

void Sequencer::setPlaying(bool on) noexcept
{
    isPlaying = on;
    if (!on)
        isRecording = false;
}

void Sequencer::setRecording(bool on) noexcept
{
    isRecording = on;
    if (on)
        isPlaying = true;
}

void Sequencer::setMuted(int slot, bool muted) noexcept
{
    if (slot < 0 || slot >= 32)
        return;
    const auto bit = 1u << slot;
    mutes = muted ? (mutes | bit) : (mutes & ~bit);
}

void Sequencer::erase() noexcept
{
    seq.count = 0;
}

void Sequencer::eraseSlot(int slot) noexcept
{
    int kept = 0;
    for (int i = 0; i < seq.count; ++i)
        if (seq.events[static_cast<std::size_t>(i)].slot != slot)
        {
            seq.events[static_cast<std::size_t>(kept)] = seq.events[static_cast<std::size_t>(i)];
            recordedInPass[static_cast<std::size_t>(kept)] = recordedInPass[static_cast<std::size_t>(i)];
            ++kept;
        }
    seq.count = kept;
}

double Sequencer::positionAt(double beat) const noexcept
{
    const double len = seq.lengthBeats();
    const double p = std::fmod(beat, len);
    return p < 0.0 ? p + len : p;
}

void Sequencer::record(int slot, bool release, double beat) noexcept
{
    if (!isRecording || slot < 0 || slot >= Sampler::kSlots || seq.count >= kMaxSequenceEvents)
        return;
    const double len = seq.lengthBeats();
    const double pos = positionAt(beat);
    const double rounded = std::round(pos / kGrid) * kGrid; // may be `len` itself: the next pass's start
    // The pass the quantized point falls in (the live press was heard there; it isn't played again in it).
    const auto pass = static_cast<std::int64_t>(std::floor((beat + (rounded - pos)) / len + 1e-9));
    const double at = rounded >= len ? rounded - len : rounded;
    for (int i = 0; i < seq.count; ++i) // the same slot and kind on the same 1/16 already: once is enough
    {
        const auto& e = seq.events[static_cast<std::size_t>(i)];
        if (e.slot == slot && e.kind == (release ? 1 : 0) && std::abs(e.beat - static_cast<float>(at)) < 1e-4f)
            return;
    }
    seq.events[static_cast<std::size_t>(seq.count)] = {static_cast<float>(at), static_cast<std::uint8_t>(slot),
                                                       static_cast<std::uint8_t>(release ? 1 : 0)};
    recordedInPass[static_cast<std::size_t>(seq.count)] = pass;
    ++seq.count;
}

void Sequencer::process(Sampler& sampler, double beatStart, double beatEnd, double samplesPerBeat,
                        int numSamples) noexcept
{
    if (!isPlaying || seq.count == 0 || !(beatEnd > beatStart))
        return;
    const double len = seq.lengthBeats();
    for (int i = 0; i < seq.count; ++i)
    {
        const auto& e = seq.events[static_cast<std::size_t>(i)];
        if ((mutes & (1u << e.slot)) != 0)
            continue;
        // The first time at or after beatStart that the master clock is at this event (mod the length).
        const double pass = std::ceil((beatStart - e.beat) / len - 1e-9);
        const double at = pass * len + e.beat;
        if (at >= beatEnd)
            continue;
        if (recordedInPass[static_cast<std::size_t>(i)] == static_cast<std::int64_t>(pass))
            continue; // recorded in this pass: it was heard live
        const int offset = std::clamp(static_cast<int>(std::lround((at - beatStart) * samplesPerBeat)), 0, numSamples - 1);
        if (e.kind == 0)
            sampler.press(e.slot, offset);
        else
            sampler.releasePad(e.slot);
    }
}

} // namespace devildj::engine
