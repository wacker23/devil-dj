#pragma once

#include <cstdint>

namespace devildj::engine
{

// How the master and headphone-cue buses reach the device outputs (P1-03).
enum class OutputLayout : std::uint8_t
{
    StereoMaster = 0, // outputs 1/2 = master L/R; no cue
    FourChannel = 1,  // outputs 1/2 = master L/R, outputs 3/4 = cue L/R (needs >= 4 device outputs)
    SplitMono = 2,    // one stereo output: left = master summed to mono, right = cue summed to mono
    SeparateCueDevice = 3, // P2-25: master on this device's 1/2; the cue on another device (engine/CueDevice)
};

constexpr int kNumOutputLayouts = 4;

// Number of device output channels a layout needs.
constexpr int outputChannelsFor(OutputLayout layout) noexcept
{
    return layout == OutputLayout::FourChannel ? 4 : 2;
}

// True if the cue bus is audible with this layout on a device with 'numOutputs' active outputs.
constexpr bool cueIsRouted(OutputLayout layout, int numOutputs) noexcept
{
    switch (layout)
    {
        case OutputLayout::StereoMaster:
            return false;
        case OutputLayout::FourChannel:
            return numOutputs >= 4;
        case OutputLayout::SplitMono:
            return numOutputs >= 2;
        case OutputLayout::SeparateCueDevice:
            return true; // on the other device (whether it is open, CueDevice says)
    }
    return false;
}

const char* outputLayoutName(OutputLayout layout) noexcept;

// Copies the master and cue buses (stereo, numSamples each) to the device outputs for 'layout'.
// Pure function, audio-thread safe. Outputs that the layout doesn't use are cleared; null output pointers are
// skipped. Mono sums use 0.5 · (L + R) so a full-scale stereo signal can't clip.
// Fallbacks: FourChannel on a device with < 4 outputs plays the master only; any layout on a 1-output device
// plays the master summed to mono.
void routeOutputs(OutputLayout layout, const float* masterL, const float* masterR, const float* cueL, const float* cueR,
                  float* const* outputs, int numOutputs, int numSamples) noexcept;

} // namespace devildj::engine
