#include "MemoryCuePanel.h"

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kText(0xffe8e8ee);
const juce::Colour kDim(0xff9a9aa6);
const juce::Colour kAccent(0xffe0483c);
} // namespace

MemoryCuePanel::MemoryCuePanel()
{
    listBox.setModel(this);
    listBox.setRowHeight(26);
    listBox.setColour(juce::ListBox::backgroundColourId, kBackground);
    addAndMakeVisible(listBox);
    for (auto* b : {&add, &rename, &remove})
    {
        b->setColour(juce::TextButton::buttonColourId, kPanel);
        b->setColour(juce::TextButton::textColourOffId, kText);
        addAndMakeVisible(b);
    }
    add.setTooltip("Store a memory cue at the playhead (Ctrl+Z / Ctrl+M)");
    add.onClick = [this]
    {
        if (onAdd)
            onAdd();
    };
    rename.onClick = [this]
    {
        if (onRename && listBox.getSelectedRow() >= 0)
            onRename(listBox.getSelectedRow());
    };
    remove.onClick = [this]
    {
        if (onDelete && listBox.getSelectedRow() >= 0)
            onDelete(listBox.getSelectedRow());
    };
    rename.setEnabled(false);
    remove.setEnabled(false);
    setSize(340, 320);
}

void MemoryCuePanel::setRows(std::vector<Row> newRows, const juce::String& title)
{
    rows = std::move(newRows);
    heading = title;
    listBox.updateContent();
    if (listBox.getSelectedRow() >= static_cast<int>(rows.size()))
        listBox.deselectAllRows();
    selectedRowsChanged(listBox.getSelectedRow());
    repaint();
}

void MemoryCuePanel::resized()
{
    auto area = getLocalBounds().reduced(8);
    area.removeFromTop(24); // heading
    auto buttons = area.removeFromBottom(30);
    area.removeFromBottom(6);
    listBox.setBounds(area);
    const int w = (buttons.getWidth() - 2 * 6) / 3;
    add.setBounds(buttons.removeFromLeft(w));
    buttons.removeFromLeft(6);
    rename.setBounds(buttons.removeFromLeft(w));
    buttons.removeFromLeft(6);
    remove.setBounds(buttons);
}

void MemoryCuePanel::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    g.setColour(kText);
    g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    g.drawText(heading, getLocalBounds().reduced(10, 8).removeFromTop(20), juce::Justification::centredLeft);
    if (rows.empty())
    {
        g.setColour(kDim);
        g.setFont(juce::FontOptions(13.0f));
        g.drawFittedText("No memory cues yet. Pause where you want one and press Add here (Ctrl+Z / Ctrl+M).",
                         listBox.getBounds().reduced(8), juce::Justification::centred, 3);
    }
}

void MemoryCuePanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= static_cast<int>(rows.size()))
        return;
    const auto& r = rows[static_cast<std::size_t>(row)];
    g.fillAll(selected ? kAccent.withAlpha(0.35f) : (row % 2 == 0 ? kPanel : kBackground));
    auto area = juce::Rectangle<int>(0, 0, width, height).reduced(6, 0);
    const auto marker = r.colour ? juce::Colour(0xff000000u | *r.colour) : juce::Colours::white.withAlpha(0.7f);
    g.setColour(marker);
    g.fillRoundedRectangle(area.removeFromLeft(6).reduced(0, 6).toFloat(), 2.0f);
    area.removeFromLeft(8);
    g.setColour(kText);
    g.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    g.drawText(r.time, area.removeFromLeft(70), juce::Justification::centredLeft);
    g.setColour(r.loop ? juce::Colour(0xff3cc46a) : kDim);
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    g.drawText(r.loop ? "LOOP" : "CUE", area.removeFromLeft(40), juce::Justification::centredLeft);
    g.setColour(kText);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(r.label, area, juce::Justification::centredLeft, true);
}

void MemoryCuePanel::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    callRow(row);
}

void MemoryCuePanel::callRow(int row)
{
    if (onCall && row >= 0 && row < static_cast<int>(rows.size()))
        onCall(row);
}

void MemoryCuePanel::selectedRowsChanged(int row)
{
    const bool any = row >= 0 && row < static_cast<int>(rows.size());
    rename.setEnabled(any);
    remove.setEnabled(any);
}

} // namespace devildj::ui
