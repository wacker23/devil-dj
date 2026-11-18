#pragma once

#include "engine/AudioEngine.h"
#include "ui/MixerControls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>

namespace devildj::ui
{

// Mixer section between the decks (P1-15): per channel (A, B) TRIM, HI / MID / LOW, FILTER, CUE, channel
// fader with a pre-fader meter; in the middle MASTER level with the master meter, headphone MIX and LEVEL;
// the crossfader along the bottom. Every control sends SetParam and shows the engine's current value each
// frame (so keyboard changes and kills show up), except while the user is moving it.
class MixerView final : public juce::Component
{
public:
    explicit MixerView(engine::AudioEngine& engine);
    ~MixerView() override;

    static constexpr int kChannels = 2;
    static constexpr int kPreferredWidth = 372;
    static constexpr int kPreferredHeight = 430; // full-size knobs and faders
    static constexpr int kMinimumHeight = 330;   // knob rows shrink, faders keep ~70 px

    void resized() override;
    void paint(juce::Graphics& g) override;
    void refresh(); // pull the engine state into the controls (every frame; public for tests)

    // Knob <-> parameter mappings (0..1 knob positions; default 0.5 = 0 dB / off).
    static double trimDb(double knob) noexcept;
    static double trimKnob(double db) noexcept;
    static double eqDb(double knob, bool isolator) noexcept; // left: EQ -26 dB / isolator kill; right: +6 dB
    static double eqKnob(double db, bool isolator) noexcept;
    static double levelDb(double knob) noexcept; // master / headphones: left = silent, centre 0 dB, right +6
    static double levelKnob(double db) noexcept;

    struct Channel
    {
        std::unique_ptr<ValueControl> trim, hi, mid, low, filter, fader;
        std::unique_ptr<juce::TextButton> cue;
        std::unique_ptr<LevelMeterView> meter;
    };
    Channel& channel(int index) noexcept { return channels[static_cast<std::size_t>(index)]; }
    ValueControl& crossfader() noexcept { return *xfader; }
    ValueControl& master() noexcept { return *masterLevel; }
    ValueControl& headphoneMix() noexcept { return *cueMix; }
    ValueControl& headphoneLevel() noexcept { return *phonesLevel; }

private:
    void send(engine::ParamId id, double value);
    static void show(ValueControl& c, double v);

    engine::AudioEngine& engine;
    std::array<Channel, kChannels> channels;
    std::unique_ptr<ValueControl> xfader, masterLevel, cueMix, phonesLevel;
    std::unique_ptr<LevelMeterView> masterMeter;
    std::array<juce::Rectangle<int>, kChannels> headers;
    juce::VBlankAttachment vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerView)
};

} // namespace devildj::ui
