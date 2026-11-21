#include "engine/OutputRouter.h"

#include <algorithm>

namespace devildj::engine
{

namespace
{
void copy(float* dst, const float* src, int n) noexcept
{
    if (dst != nullptr)
        std::copy(src, src + n, dst);
}

void monoSum(float* dst, const float* l, const float* r, int n) noexcept
{
    if (dst == nullptr)
        return;
    for (int i = 0; i < n; ++i)
        dst[i] = 0.5f * (l[i] + r[i]);
}

void clear(float* dst, int n) noexcept
{
    if (dst != nullptr)
        std::fill(dst, dst + n, 0.0f);
}
} // namespace

const char* outputLayoutName(OutputLayout layout) noexcept
{
    switch (layout)
    {
        case OutputLayout::StereoMaster:
            return "Stereo (master only)";
        case OutputLayout::FourChannel:
            return "4-channel (master 1/2, cue 3/4)";
        case OutputLayout::SplitMono:
            return "Split (master left, cue right)";
        case OutputLayout::SeparateCueDevice:
            return "Separate headphone device (master here, cue there)";
    }
    return "?";
}

void routeOutputs(OutputLayout layout, const float* masterL, const float* masterR, const float* cueL, const float* cueR,
                  float* const* outputs, int numOutputs, int numSamples) noexcept
{
    if (numOutputs <= 0 || numSamples <= 0)
        return;

    for (int ch = 0; ch < numOutputs; ++ch)
        clear(outputs[ch], numSamples);

    if (numOutputs == 1)
    {
        monoSum(outputs[0], masterL, masterR, numSamples);
        return;
    }

    switch (layout)
    {
        case OutputLayout::StereoMaster:
        case OutputLayout::SeparateCueDevice: // the cue goes to the other device (AudioEngine::separateCueOutput)
            copy(outputs[0], masterL, numSamples);
            copy(outputs[1], masterR, numSamples);
            break;

        case OutputLayout::FourChannel:
            copy(outputs[0], masterL, numSamples);
            copy(outputs[1], masterR, numSamples);
            if (numOutputs >= 4)
            {
                copy(outputs[2], cueL, numSamples);
                copy(outputs[3], cueR, numSamples);
            }
            break;

        case OutputLayout::SplitMono:
            monoSum(outputs[0], masterL, masterR, numSamples);
            monoSum(outputs[1], cueL, cueR, numSamples);
            break;
    }
}

} // namespace devildj::engine
