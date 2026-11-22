#pragma once

#include "engine/AudioEngine.h"
#include "engine/Sampler.h"
#include "ui/MixerControls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>

namespace devildj::ui
{

// The sampler deck (P3-13): the current bank's 8 slots - each with its name, PLAY (hold: a gate plays while held;
// one-shot and loop on the press), the mode (ONE / LOOP / GATE), SYNC (to the master tempo) and a gain knob;
// right-click a slot to load a file or the browser's selected track, or empty it - and the sampler's own channel:
// LEVEL, FILTER (the color FX knob), CUE, and its crossfader side (A / THRU / B). Banks 1-4. The deck pads play the
// same bank in Sampler pad mode (Shift+F4). Mouse, touch; the engine is driven directly, loading goes to the app.
class SamplerPanel final : public juce::Component
{
public:
    static constexpr int kPreferredHeight = 96;

    explicit SamplerPanel(engine::AudioEngine& engine);
    ~SamplerPanel() override;

    std::function<void(int slot)> onLoadFile;     // choose a file for the slot (0..31)
    std::function<void(int slot)> onLoadSelected; // the browser's selected track
    std::function<void(int slot)> onClear;
    std::function<void(int bank)> onBank;         // the bank shown changed (the pads follow)

    void setSlotName(int slot, const juce::String& name);
    void setBank(int bank);
    int bank() const noexcept { return currentBank; }

    void refresh(); // from the engine state (every frame; public for tests)
    void resized() override;
    void paint(juce::Graphics& g) override;

    // A button that reports press and release.
    class HoldButton final : public juce::TextButton
    {
    public:
        std::function<void(bool down)> onHold;

    private:
        void buttonStateChanged() override
        {
            const bool down = isDown();
            if (down != wasDown && onHold)
                onHold(down);
            wasDown = down;
        }
        bool wasDown = false;
    };
    struct Tile
    {
        HoldButton play;
        juce::TextButton mode{"ONE"}, sync{"SYNC"};
        std::unique_ptr<ValueControl> gain;
        juce::Rectangle<int> area;
    };
    Tile& tile(int index) noexcept { return tiles[static_cast<std::size_t>(index)]; }
    juce::TextButton& bankButton(int bank) noexcept { return banks[static_cast<std::size_t>(bank)]; }
    juce::TextButton& cueButton() noexcept { return cue; }
    juce::TextButton& assignButton() noexcept { return assign; }
    ValueControl& levelControl() noexcept { return *level; }
    void showSlotMenu(int tileIndex);
    void mouseDown(const juce::MouseEvent& e) override;

private:
    void sendParam(engine::ChannelParam p, float value);
    int slotOf(int tileIndex) const noexcept { return currentBank * engine::Sampler::kSlotsPerBank + tileIndex; }

    engine::AudioEngine& engine;
    std::array<Tile, engine::Sampler::kSlotsPerBank> tiles;
    std::array<juce::TextButton, engine::Sampler::kBanks> banks;
    std::array<juce::String, engine::Sampler::kSlots> names;
    juce::TextButton cue{"CUE"}, assign{"THRU"};
    std::unique_ptr<ValueControl> level, filter;
    int currentBank = 0;
    juce::VBlankAttachment vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SamplerPanel)
};

} // namespace devildj::ui
