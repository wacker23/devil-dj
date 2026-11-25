#pragma once

// "What you care about" (P6-12; MATCHER_SPEC §7.2): the Track Matcher's weights as the personal ranker has them -
// one bar per feature (the blend in use, the preset's as an outline), how many choices it learnt from and how much
// they count (alpha), with TRAIN NOW and RESET.

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace devildj::ui
{

class CareView final : public juce::Component
{
public:
    static constexpr int kDims = 14;
    struct State
    {
        std::array<double, kDims> preset{}, inUse{};
        std::array<juce::String, kDims> names;
        juce::String presetName;
        int impressions = 0, logged = 0; // what the model learnt from / what the log holds now
        double alpha = 0.0;
        bool trained = false;
        juce::String trainedWhen;
    };

    CareView();
    void setState(State s);
    const State& state() const noexcept { return current; }
    std::function<void()> onTrain, onReset;

    void paint(juce::Graphics& g) override;
    void resized() override;
    juce::TextButton& trainButton() noexcept { return train; }
    juce::TextButton& resetButton() noexcept { return reset; }
    // The bars, largest first (as shown): indices into the arrays.
    std::array<int, kDims> order() const;

private:
    State current;
    juce::TextButton train{"TRAIN NOW"}, reset{"RESET"};
    juce::Rectangle<int> barsArea;
};

} // namespace devildj::ui
