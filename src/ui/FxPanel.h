#pragma once

#include "dsp/fx/BeatFx.h"
#include "engine/AudioEngine.h"
#include "ui/MixerControls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace devildj::ui
{

// The two FX units (P3-09; the framework is P3-08). Each unit: its target (deck A / B or the master), multi /
// single mode, the beat length (1/16 ... 16, for every slot), TAP (a tapped tempo instead of the master clock;
// right-click: back to the clock), and its slots - the effect (click: choose), ON, the dry / wet knob and the
// effect's first knob (single mode: slot 1 with up to three knobs). The panel sends the engine commands itself;
// choosing an effect goes to the app (onEffect), which creates and installs it. Mouse, touch and the keys
// (F5-F8, Shift+F5 / Shift+F7: MainComponent).
class FxPanel final : public juce::Component
{
public:
    static constexpr int kPreferredHeight = 96;
    static constexpr std::array<double, 9> kBeatSteps{1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1, 2, 4, 8, 16};

    explicit FxPanel(engine::AudioEngine& engine);
    ~FxPanel() override;

    // The app installs effects: -1 = none.
    std::function<void(int unit, int slot, int type)> onEffect;
    // What is installed (the panel shows its name and knobs; the knobs go back to the effect's defaults).
    void setEffectType(int unit, int slot, int type);
    int effectType(int unit, int slot) const noexcept { return types[static_cast<std::size_t>(unit)][static_cast<std::size_t>(slot)]; }

    void refresh(); // from the engine state (every frame; public for tests)
    void resized() override;
    void paint(juce::Graphics& g) override;

    // Actions also bound to keys.
    void toggleUnit(int unit);  // F5 / F7: on (the slots with an effect) / all off
    void nextEffect(int unit);  // F6 / F8: slot 1's next effect
    void cycleTarget(int unit); // Shift+F5 / Shift+F7: deck A -> deck B -> master
    void setBeats(int unit, double beats);
    void tap(int unit, double timeSeconds); // a TAP press at this time (seconds, any steady clock)

    struct Slot
    {
        juce::TextButton effect, on{"ON"};
        std::unique_ptr<ValueControl> wet;
        std::array<std::unique_ptr<ValueControl>, 3> knobs; // the effect's parameters (multi mode: the first)
    };
    struct Unit
    {
        juce::TextButton target, mode{"MULTI"}, beatsDown, beatsUp, beats{"1"}, tapButton{"TAP"};
        std::array<Slot, engine::kFxSlots> slots;
        std::vector<double> taps;
    };
    Unit& unit(int index) noexcept { return units[static_cast<std::size_t>(index)]; }
    static juce::String beatsText(double beats);
    static juce::String targetText(engine::FxTarget target);

private:
    void sendFx(int unit, engine::FxOp op, int slot, int index, float value);
    void showEffectMenu(int unit, int slot);
    void showTargetMenu(int unit);
    void layoutUnit(Unit& u, juce::Rectangle<int> area, bool single);

    engine::AudioEngine& engine;
    std::array<Unit, engine::kFxUnits> units;
    std::array<std::array<int, engine::kFxSlots>, engine::kFxUnits> types{};
    std::array<double, engine::kFxUnits> beats{1.0, 1.0};
    std::array<bool, engine::kFxUnits> single{};
    std::array<juce::Rectangle<int>, engine::kFxUnits> unitAreas;
    // Each effect type's parameter names and defaults (asked once from a throwaway instance).
    std::array<std::vector<dsp::fx::EffectParam>, dsp::fx::kEffectTypeCount> params;
    juce::VBlankAttachment vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxPanel)
};

} // namespace devildj::ui
