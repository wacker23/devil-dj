#include "ui/NextTrackPanel.h"

#include "util/MusicalKey.h"

#include <cmath>

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kText(0xffe8e8ee);
const juce::Colour kDim(0xff8a8a96);
const juce::Colour kAccent(0xffe0483c);
const juce::Colour kGood(0xff3cc46a), kWeak(0xffe0a030), kClash(0xffe0483c);
} // namespace

NextTrackPanel::NextTrackPanel()
{
    scope.addItem("Whole library", 1);
    scope.addItem("The playlist shown", 2);
    scope.onChange = [this]
    {
        if (onScope)
            onScope(scope.getSelectedId() - 1);
    };
    keyPolicy.addItem("Key: strict", 1);
    keyPolicy.addItem("Key: relaxed", 2);
    keyPolicy.addItem("Key: any", 3);
    keyPolicy.onChange = [this]
    {
        if (onKeyPolicy)
            onKeyPolicy(keyPolicy.getSelectedId() - 1);
    };
    preset.onChange = [this]
    {
        if (onPreset)
            onPreset(preset.getSelectedId() - 1);
    };
    scope.setTooltip("Where to look (Ctrl+Alt+N)");
    preset.setTooltip("What matters most (Alt+N)");
    keyPolicy.setTooltip("How strictly the keys must fit (Alt+Shift+N)");
    for (auto* c : {&scope, &preset, &keyPolicy})
        addAndMakeVisible(*c);
    for (auto* b : {&thumbsUp, &thumbsDown}) // P6-11
    {
        const bool up = b == &thumbsUp;
        b->setColour(juce::TextButton::buttonColourId, kPanel);
        b->setColour(juce::TextButton::textColourOffId, up ? juce::Colour(0xff3cc46a) : juce::Colour(0xffe0483c));
        b->setTooltip(up ? "The last transition was good (') - the Track Matcher learns from it"
                         : "The last transition was bad (Shift+') - the Track Matcher learns from it");
        b->setWantsKeyboardFocus(false);
        b->onClick = [this, up]
        {
            if (onThumbs)
                onThumbs(up);
        };
        addAndMakeVisible(*b);
    }
    const char* names[] = {"\xe2\x86\x93 DOWN", "\xe2\x86\x92 HOLD", "\xe2\x86\x91 UP", "\xe2\xa4\xa8 SWITCH"};
    for (int i = 0; i < 4; ++i)
    {
        auto& b = intents[static_cast<std::size_t>(i)];
        b.setButtonText(juce::String(juce::CharPointer_UTF8(names[i])));
        b.setColour(juce::TextButton::buttonColourId, kPanel);
        b.setColour(juce::TextButton::buttonOnColourId, kAccent);
        b.setColour(juce::TextButton::textColourOffId, kText);
        b.setTooltip("Intent: cool down / hold the energy / build / change style (Ctrl+Shift+N cycles)");
        b.setWantsKeyboardFocus(false);
        b.onClick = [this, i]
        {
            if (onIntent)
                onIntent(i);
        };
        addAndMakeVisible(b);
    }
    setState({});
}

int NextTrackPanel::chipLevel(double v)
{
    return v >= 0.7 ? 0 : v >= 0.4 ? 1 : 2;
}

void NextTrackPanel::setState(State s)
{
    current = std::move(s);
    preset.clear(juce::dontSendNotification);
    for (int i = 0; i < current.presets.size(); ++i)
        preset.addItem(current.presets[i], i + 1);
    preset.setSelectedId(current.preset + 1, juce::dontSendNotification);
    scope.setSelectedId(current.scope + 1, juce::dontSendNotification);
    keyPolicy.setSelectedId(current.keyPolicy + 1, juce::dontSendNotification);
    for (int i = 0; i < 4; ++i)
        intents[static_cast<std::size_t>(i)].setToggleState(i == current.intent, juce::dontSendNotification);
    repaint();
}

int NextTrackPanel::rowAt(juce::Point<int> p) const
{
    if (!listArea.contains(p))
        return -1;
    const int row = (p.y - listArea.getY()) / kRowHeight;
    return row < static_cast<int>(current.rows.size()) ? row : -1;
}

void NextTrackPanel::mouseDown(const juce::MouseEvent& e)
{
    const int row = rowAt(e.getPosition());
    if (row >= 0 && onHighlight)
        onHighlight(row);
}

void NextTrackPanel::mouseDoubleClick(const juce::MouseEvent& e)
{
    const int row = rowAt(e.getPosition());
    if (row >= 0 && onLoad)
        onLoad(row);
}

void NextTrackPanel::mouseMove(const juce::MouseEvent& e)
{
    const int row = rowAt(e.getPosition());
    setTooltip(row >= 0 ? current.rows[static_cast<std::size_t>(row)].breakdown : juce::String());
}

void NextTrackPanel::resized()
{
    auto area = getLocalBounds().reduced(6);
    auto top = area.removeFromTop(24);
    scope.setBounds(top.removeFromLeft(top.getWidth() / 3).reduced(1, 0));
    preset.setBounds(top.removeFromLeft(top.getWidth() / 2).reduced(1, 0));
    keyPolicy.setBounds(top.reduced(1, 0));
    area.removeFromTop(4);
    auto row = area.removeFromTop(24);
    thumbsDown.setBounds(row.removeFromRight(28).reduced(1, 0)); // P6-11
    thumbsUp.setBounds(row.removeFromRight(28).reduced(1, 0));
    row.removeFromRight(4);
    const int w = row.getWidth() / 4;
    for (auto& b : intents)
        b.setBounds(row.removeFromLeft(w).reduced(1, 0));
    area.removeFromTop(4);
    miniArea = area.removeFromBottom(std::min(110, area.getHeight() / 3));
    area.removeFromBottom(4);
    listArea = area.removeFromTop(std::min(area.getHeight(), 10 * kRowHeight));
}

void NextTrackPanel::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    if (current.rows.empty())
    {
        g.setColour(kDim);
        g.setFont(juce::FontOptions(13.0f));
        g.drawFittedText(current.context.isNotEmpty() ? current.context
                                                      : juce::String("Play a track from the library: suggestions for "
                                                                     "the other deck appear here."),
                         listArea.reduced(8), juce::Justification::centredTop, 4);
    }
    for (std::size_t i = 0; i < current.rows.size(); ++i)
    {
        const auto& r = current.rows[i];
        auto box = listArea.withY(listArea.getY() + static_cast<int>(i) * kRowHeight).withHeight(kRowHeight - 2);
        if (box.getBottom() > listArea.getBottom())
            break;
        g.setColour(static_cast<int>(i) == current.highlight ? kAccent.withAlpha(0.3f) : kPanel);
        g.fillRoundedRectangle(box.toFloat(), 3.0f);
        // The score bar along the bottom.
        g.setColour(kGood.withAlpha(0.6f));
        g.fillRect(box.withTop(box.getBottom() - 3).withWidth(static_cast<int>(box.getWidth() * std::clamp(r.score, 0.0, 1.0))));
        auto text = box.reduced(6, 2);
        auto line1 = text.removeFromTop(15);
        g.setColour(kText);
        g.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        const juce::String info = juce::String(r.bpm, 1) + "  " + juce::String(util::camelotCode(r.key)) +
                                  (r.energy ? "  E" + juce::String(std::clamp(static_cast<int>(std::ceil(*r.energy * 10.0 - 1e-9)), 1, 10)) : juce::String());
        g.drawText(info, line1.removeFromRight(110), juce::Justification::centredRight);
        g.drawText((r.wildcard ? juce::String(juce::CharPointer_UTF8("\xe2\x9c\xa6 ")) : juce::String()) + r.title +
                       (r.artist.isNotEmpty() ? "  -  " + r.artist : juce::String()),
                   line1, juce::Justification::centredLeft, true);
        // Reason chips.
        g.setFont(juce::FontOptions(10.0f));
        auto chips = text;
        for (const auto& c : r.chips)
        {
            const int cw = std::min(chips.getWidth(), 12 + static_cast<int>(c.text.length()) * 6);
            if (cw < 20)
                break;
            const auto chip = chips.removeFromLeft(cw).reduced(1, 1);
            chips.removeFromLeft(3);
            g.setColour((c.level == 0 ? kGood : c.level == 1 ? kWeak : kClash).withAlpha(0.3f));
            g.fillRoundedRectangle(chip.toFloat(), 3.0f);
            g.setColour(kText);
            g.drawText(c.text, chip, juce::Justification::centred, true);
        }
    }
    // The mini view: the energy line and the Camelot wheel.
    auto mini = miniArea;
    auto wheel = mini.removeFromRight(mini.getHeight()).reduced(4);
    auto line = mini.reduced(6, 8);
    g.setColour(kPanel);
    g.fillRoundedRectangle(miniArea.toFloat(), 4.0f);
    std::vector<double> energies = current.energyHistory;
    const bool haveNext = current.highlight >= 0 && current.highlight < static_cast<int>(current.rows.size()) &&
                          current.rows[static_cast<std::size_t>(current.highlight)].energy.has_value();
    if (haveNext)
        energies.push_back(*current.rows[static_cast<std::size_t>(current.highlight)].energy);
    if (energies.size() >= 2)
    {
        juce::Path p;
        for (std::size_t i = 0; i < energies.size(); ++i)
        {
            const float x = line.getX() + line.getWidth() * static_cast<float>(i) / static_cast<float>(energies.size() - 1);
            const float y = line.getBottom() - line.getHeight() * static_cast<float>(std::clamp(energies[i], 0.0, 1.0));
            i == 0 ? p.startNewSubPath(x, y) : p.lineTo(x, y);
        }
        g.setColour(kText.withAlpha(0.8f));
        g.strokePath(p, juce::PathStrokeType(2.0f));
    }
    g.setColour(kDim);
    g.setFont(juce::FontOptions(10.0f));
    g.drawText("energy", line, juce::Justification::topLeft);
    // The wheel: 12 numbers, A outside / B inside; A's key and the highlighted one marked.
    const auto c = wheel.getCentre().toFloat();
    const float rOuter = wheel.getWidth() * 0.48f, rInner = rOuter * 0.62f;
    g.setColour(kDim.withAlpha(0.5f));
    g.drawEllipse(wheel.toFloat().reduced(wheel.getWidth() * 0.02f), 1.0f);
    const auto mark = [&](int key, juce::Colour colour)
    {
        if (!util::isValidKey(key))
            return;
        const int number = util::camelotNumber(key);
        const float angle = juce::MathConstants<float>::twoPi * (number - 1) / 12.0f - juce::MathConstants<float>::halfPi;
        const float radius = key >= 12 ? rInner : rOuter * 0.82f;
        g.setColour(colour);
        g.fillEllipse(c.x + radius * std::cos(angle) - 5.0f, c.y + radius * std::sin(angle) - 5.0f, 10.0f, 10.0f);
    };
    mark(current.keyA, kText);
    if (current.highlight >= 0 && current.highlight < static_cast<int>(current.rows.size()))
        mark(current.rows[static_cast<std::size_t>(current.highlight)].key, kAccent);
}

} // namespace devildj::ui
