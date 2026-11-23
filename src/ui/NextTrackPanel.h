#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <vector>

namespace devildj::ui
{

// The Next Track panel (P3-24; MATCHER_SPEC §6): what to play next on the idle deck. Header - scope (library /
// the playlist shown), preset, intent DOWN / HOLD / UP / SWITCH, key policy; the top 10 - title / artist, BPM, key,
// energy 1-10, a score bar and up to 4 reason chips (green good, amber weak, red a clash), the wildcard marked; hover
// a row for every feature's value x weight; a mini view - the energy of the last 5 tracks and the highlighted one,
// and a Camelot wheel with A and B. Clicking a row highlights it, a double-click loads it. The app fills it.
class NextTrackPanel final : public juce::Component, public juce::SettableTooltipClient
{
public:
    struct Chip
    {
        juce::String text;
        int level = 0; // 0 good, 1 weak, 2 clash
    };
    struct Row
    {
        juce::String title, artist;
        double bpm = 0.0;
        int key = -1;
        std::optional<double> energy;
        double score = 0.0;
        bool wildcard = false;
        std::vector<Chip> chips;
        juce::String breakdown; // the full one, for the tooltip
    };
    struct State
    {
        int scope = 0;      // 0 library, 1 the playlist shown
        juce::StringArray presets;
        int preset = 0;
        int intent = 1;     // 0 down, 1 hold, 2 up, 3 switch
        int keyPolicy = 1;  // 0 strict, 1 relaxed, 2 off
        int highlight = 0;
        std::vector<Row> rows;
        std::vector<double> energyHistory; // the last tracks played, oldest first
        int keyA = -1;
        juce::String context; // "for <title>" or why nothing
    };

    NextTrackPanel();
    void setState(State s);
    const State& state() const noexcept { return current; }

    std::function<void(int)> onScope, onPreset, onIntent, onKeyPolicy, onHighlight, onLoad;

    void resized() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;

    juce::ComboBox& scopeBox() noexcept { return scope; }
    juce::ComboBox& presetBox() noexcept { return preset; }
    juce::ComboBox& keyBox() noexcept { return keyPolicy; }
    juce::TextButton& intentButton(int i) noexcept { return intents[static_cast<std::size_t>(i)]; }
    int rowAt(juce::Point<int> p) const;
    static int chipLevel(double value); // 0 >= 0.7, 1 >= 0.4, else 2

private:
    State current;
    juce::ComboBox scope, preset, keyPolicy;
    std::array<juce::TextButton, 4> intents;
    juce::Rectangle<int> listArea, miniArea;
    static constexpr int kRowHeight = 34;
};

} // namespace devildj::ui
