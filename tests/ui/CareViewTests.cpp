// P6-12: "What you care about" - the bars largest first, TRAIN NOW / RESET and when they can be used.

#include "ui/CareView.h"

#include <catch2/catch_test_macros.hpp>

using devildj::ui::CareView;

TEST_CASE("Care view: bars in order, the buttons", "[ui][ranker]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    CareView view;
    view.setBounds(0, 0, 460, 430);
    int trained = 0, reset = 0;
    view.onTrain = [&] { ++trained; };
    view.onReset = [&] { ++reset; };

    CareView::State s;
    s.presetName = "Balanced";
    for (int k = 0; k < CareView::kDims; ++k)
    {
        s.preset[static_cast<std::size_t>(k)] = 1.0;
        s.inUse[static_cast<std::size_t>(k)] = 0.1 * k;
        s.names[static_cast<std::size_t>(k)] = "f" + juce::String(k);
    }
    s.inUse[4] = 3.0; // groove first
    view.setState(s);
    CHECK(view.order()[0] == 4);
    CHECK(view.order()[1] == 13);
    CHECK_FALSE(view.resetButton().isEnabled()); // nothing learnt
    CHECK_FALSE(view.trainButton().isEnabled()); // nothing logged

    s.logged = 12;
    s.trained = true;
    s.impressions = 12;
    s.alpha = 12.0 / 212.0;
    view.setState(s);
    CHECK(view.trainButton().isEnabled());
    CHECK(view.resetButton().isEnabled());
    view.trainButton().onClick();
    view.resetButton().onClick();
    CHECK(trained == 1);
    CHECK(reset == 1);
    // It paints without trouble.
    juce::Image image(juce::Image::ARGB, 460, 430, true);
    juce::Graphics g(image);
    view.paint(g);
}
