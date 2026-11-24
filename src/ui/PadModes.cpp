#include "ui/PadModes.h"

#include <algorithm>
#include <cmath>

namespace devildj::ui
{

namespace
{
constexpr std::uint32_t kLoopColour = 0x3CC46A;
constexpr std::uint32_t kJumpColour = 0x3C7CE0;
constexpr std::uint32_t kKeyColour = 0x9A5CE0;
constexpr std::uint32_t kKeyboardColour = 0xE08A3C;
constexpr std::uint32_t kSlicerColour = 0x30C0C0;

int clampPad(int pad) noexcept { return std::clamp(pad, 0, 7); }

juce::String beatsLabel(double beats)
{
    if (beats < 1.0)
        return "1/" + juce::String(juce::roundToInt(1.0 / beats));
    return juce::String(juce::roundToInt(beats));
}

juce::String semitoneLabel(int n)
{
    return n > 0 ? "+" + juce::String(n) : juce::String(n);
}
} // namespace

const char* padModeName(PadMode mode) noexcept
{
    switch (mode)
    {
        case PadMode::HotCue:
            return "HOT CUE";
        case PadMode::BeatLoop:
            return "BEAT LOOP";
        case PadMode::BeatJump:
            return "BEAT JUMP";
        case PadMode::Slicer:
            return "SLICER";
        case PadMode::KeyShift:
            return "KEY SHIFT";
        case PadMode::Keyboard:
            return "KEYBOARD";
        case PadMode::PadFx:
            return "PAD FX";
        case PadMode::Sampler:
            return "SAMPLER";
        case PadMode::SequenceCall:
            return "SEQ CALL";
        case PadMode::BeatPatterns:
            return "PATTERNS";
        case PadMode::BeatPads:
            return "BEAT PADS";
        case PadMode::DrumSwap:
            return "DRUM SWAP";
    }
    return "";
}

const char* padModeComesWith(PadMode mode) noexcept
{
    (void)mode; // every mode works since P3-13 (kept for modes still to come)
    return nullptr;
}

double beatLoopBeats(int pad) noexcept
{
    static constexpr double sizes[] = {0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0};
    return sizes[clampPad(pad)];
}

double beatJumpBeats(int pad) noexcept
{
    static constexpr double sizes[] = {1.0, 1.0, 2.0, 2.0, 4.0, 4.0, 8.0, 8.0};
    const int p = clampPad(pad);
    return p % 2 == 0 ? -sizes[p] : sizes[p];
}

int padSemitones(int pad) noexcept
{
    static constexpr int shifts[] = {0, 1, 2, 3, -4, -3, -2, -1};
    return shifts[clampPad(pad)];
}

PadLook padLook(PadMode mode, int pad, const engine::DeckState& deck, bool held)
{
    PadLook look;
    switch (mode)
    {
        case PadMode::BeatLoop:
        {
            const double beats = beatLoopBeats(pad);
            look.text = beatsLabel(beats);
            look.colour = kLoopColour;
            look.lit = held || (deck.loopActive && std::abs(deck.loopBeats - beats) < 1e-3);
            look.enabled = deck.loaded && deck.hasGrid;
            break;
        }
        case PadMode::BeatJump:
        {
            const double beats = beatJumpBeats(pad);
            const auto n = beatsLabel(std::abs(beats));
            look.text = beats < 0.0 ? juce::String(juce::CharPointer_UTF8("\xe2\x97\x80 ")) + n
                                    : n + juce::String(juce::CharPointer_UTF8(" \xe2\x96\xb6"));
            look.colour = kJumpColour;
            look.lit = held;
            look.enabled = deck.loaded && deck.hasGrid;
            break;
        }
        case PadMode::KeyShift:
        case PadMode::Keyboard:
        {
            const int n = padSemitones(pad);
            const bool keyboard = mode == PadMode::Keyboard;
            look.text = keyboard ? "A " + semitoneLabel(n) : semitoneLabel(n);
            look.colour = keyboard ? kKeyboardColour : kKeyColour;
            const bool current = deck.keyLock ? deck.keyShift == n : n == 0;
            look.lit = held || (current && (keyboard ? deck.hotCueStart[0] >= 0.0 : true));
            look.enabled = deck.loaded;
            break;
        }
        case PadMode::Slicer:
            look.text = juce::String(pad + 1);
            look.colour = kSlicerColour;
            look.lit = held || deck.slicerRolling == pad || (deck.slicerRolling < 0 && deck.slicerCurrent == pad);
            look.enabled = deck.loaded && deck.hasGrid && deck.slicerOn;
            break;
        case PadMode::HotCue:
        case PadMode::PadFx:
        case PadMode::Sampler:
        case PadMode::SequenceCall:
        case PadMode::BeatPatterns:
        case PadMode::BeatPads:
        case PadMode::DrumSwap:
            look.text = "-";
            look.enabled = false;
            break;
    }
    return look;
}

std::vector<engine::EngineCommand> padCommands(PadMode mode, engine::DeckId deck, int pad, int hotCuePage,
                                               bool alternate, bool release, const engine::DeckState& state)
{
    std::vector<engine::EngineCommand> out;
    pad = clampPad(pad);
    switch (mode)
    {
        case PadMode::HotCue:
        {
            const auto slot = static_cast<std::uint8_t>(std::clamp(hotCuePage, 0, 1) * 8 + pad);
            const auto op = alternate ? engine::HotCueOp::Delete
                            : release ? engine::HotCueOp::Release
                                      : engine::HotCueOp::Press;
            if (!(alternate && release))
                out.push_back(engine::HotCue{deck, slot, op});
            break;
        }
        case PadMode::BeatLoop:
            if (!release && !alternate && state.hasGrid)
                out.push_back(engine::Loop{deck, engine::LoopOp::Beat, beatLoopBeats(pad)});
            break;
        case PadMode::BeatJump:
            if (!release && !alternate && state.hasGrid)
                out.push_back(engine::BeatJump{deck, beatJumpBeats(pad)});
            break;
        case PadMode::KeyShift:
            if (release)
                break;
            if (!state.keyLock)
                out.push_back(engine::SetKeyLock{deck, true});
            out.push_back(engine::SetKeyShift{deck, alternate ? 0 : padSemitones(pad)});
            break;
        case PadMode::Keyboard:
            if (alternate)
            {
                if (!release)
                    out.push_back(engine::SetKeyShift{deck, 0});
                break;
            }
            if (release)
            {
                out.push_back(engine::HotCue{deck, 0, engine::HotCueOp::Release});
                break;
            }
            if (state.hotCueStart[0] >= 0.0) // hot cue A at this pitch (empty: the press sets it, as in Hot Cue)
            {
                if (!state.keyLock)
                    out.push_back(engine::SetKeyLock{deck, true});
                out.push_back(engine::SetKeyShift{deck, padSemitones(pad)});
            }
            out.push_back(engine::HotCue{deck, 0, engine::HotCueOp::Press});
            break;
        case PadMode::Slicer:
            if (alternate)
            {
                if (!release && pad < 3)
                    out.push_back(engine::SetSlicer{deck, true, pad == 0 ? 8.0 : pad == 1 ? 16.0 : 32.0});
                break;
            }
            out.push_back(engine::SlicerPad{deck, static_cast<std::uint8_t>(pad), release});
            break;
        case PadMode::PadFx:
        case PadMode::Sampler:
        case PadMode::SequenceCall:
        case PadMode::BeatPatterns:
        case PadMode::BeatPads:
        case PadMode::DrumSwap:
            break;
    }
    return out;
}

} // namespace devildj::ui
