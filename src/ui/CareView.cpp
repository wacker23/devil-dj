#include "ui/CareView.h"

#include <algorithm>
#include <numeric>

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff17181c), kText(0xffd8dae0), kDim(0xff7a7f8c), kBar(0xff8fd0ff);
constexpr int kRow = 22;
} // namespace

CareView::CareView()
{
    train.setTooltip("Learn from every choice logged so far (it also happens when Devil-DJ closes)");
    reset.setTooltip("Forget what was learnt: back to the preset's weights (the choice log is kept)");
    train.onClick = [this]
    {
        if (onTrain)
            onTrain();
    };
    reset.onClick = [this]
    {
        if (onReset)
            onReset();
    };
    addAndMakeVisible(train);
    addAndMakeVisible(reset);
    setSize(460, 120 + kDims * kRow);
}

void CareView::setState(State s)
{
    current = std::move(s);
    reset.setEnabled(current.trained);
    train.setEnabled(current.logged > 0);
    repaint();
}

std::array<int, CareView::kDims> CareView::order() const
{
    std::array<int, kDims> idx{};
    std::iota(idx.begin(), idx.end(), 0);
    std::stable_sort(idx.begin(), idx.end(), [this](int a, int b)
                     { return current.inUse[static_cast<std::size_t>(a)] > current.inUse[static_cast<std::size_t>(b)]; });
    return idx;
}

void CareView::resized()
{
    auto area = getLocalBounds().reduced(10);
    auto bottom = area.removeFromBottom(28);
    reset.setBounds(bottom.removeFromRight(90));
    bottom.removeFromRight(6);
    train.setBounds(bottom.removeFromRight(110));
    area.removeFromBottom(8);
    area.removeFromTop(44);
    barsArea = area;
}

void CareView::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    auto header = getLocalBounds().reduced(10).removeFromTop(40);
    g.setColour(kText);
    g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    g.drawText("What you care about", header.removeFromTop(20), juce::Justification::centredLeft);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(12.0f));
    const juce::String line = current.trained
                                  ? "Learnt from " + juce::String(current.impressions) + " choices (" +
                                        juce::String(juce::roundToInt(100.0 * current.alpha)) + " % of the weight; the " +
                                        current.presetName + " preset the rest), " + current.trainedWhen
                                  : "Nothing learnt yet: the " + current.presetName + " preset's weights. " +
                                        juce::String(current.logged) + " choices logged.";
    g.drawText(line, header, juce::Justification::centredLeft);

    double top = 1e-9;
    for (int k = 0; k < kDims; ++k)
        top = std::max({top, current.inUse[static_cast<std::size_t>(k)], current.preset[static_cast<std::size_t>(k)]});
    auto rows = barsArea;
    for (const int k : order())
    {
        auto r = rows.removeFromTop(kRow);
        const auto i = static_cast<std::size_t>(k);
        g.setColour(kText);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(current.names[i], r.removeFromLeft(110), juce::Justification::centredLeft);
        g.setColour(kDim);
        g.drawText(juce::String(current.inUse[i], 2), r.removeFromRight(44), juce::Justification::centredRight);
        const auto bar = r.reduced(0, 5).toFloat();
        g.setColour(kBar.withAlpha(0.85f));
        g.fillRect(bar.withWidth(bar.getWidth() * static_cast<float>(current.inUse[i] / top)));
        g.setColour(kText.withAlpha(0.6f)); // the preset's, as an outline
        g.drawRect(bar.withWidth(std::max(1.0f, bar.getWidth() * static_cast<float>(current.preset[i] / top))), 1.0f);
    }
}

} // namespace devildj::ui
