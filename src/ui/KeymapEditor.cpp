#include "ui/KeymapEditor.h"

#include "ui/Actions.h"

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kText(0xffe8e8ee);
const juce::Colour kDim(0xff8a8a96);
const juce::Colour kAccent(0xffe0483c);
} // namespace

KeymapEditor::KeymapEditor(std::vector<KeyBinding> current)
    : map(std::move(current))
    , all(allCommands())
{
    filter.setTextToShowWhenEmpty("Filter actions (e.g. loop, deck B, FX)", kDim);
    filter.onTextChange = [this] { refilter(); };
    addAndMakeVisible(filter);
    list.setRowHeight(22);
    list.setColour(juce::ListBox::backgroundColourId, kPanel);
    addAndMakeVisible(list);
    for (auto* b : {&setKey, &clear, &reset, &importButton, &exportButton, &save})
    {
        b->setColour(juce::TextButton::buttonColourId, kPanel);
        b->setColour(juce::TextButton::textColourOffId, kText);
        addAndMakeVisible(*b);
    }
    setKey.setTooltip("Then press the key, with Shift / Ctrl / Alt if you like (CLEAR takes a key away)");
    setKey.onClick = [this] { beginCapture(); };
    clear.onClick = [this]
    {
        if (const auto c = selected())
        {
            map = clearKey(std::move(map), *c);
            status = juce::String(commandName(*c)) + ": no key";
            list.repaint();
            repaint();
        }
    };
    reset.setTooltip("This action's keys from the default map");
    reset.onClick = [this]
    {
        if (const auto c = selected())
        {
            map = clearKey(std::move(map), *c);
            for (const auto& b : defaultKeymap())
                if (b.command == *c)
                    map = assignKey(std::move(map), *c, b.chord, b.context).bindings;
            status = juce::String(commandName(*c)) + ": " + keysOf(map, *c);
            list.repaint();
            repaint();
        }
    };
    importButton.onClick = [this]
    {
        if (onImport)
            onImport();
    };
    exportButton.onClick = [this]
    {
        if (onExport)
            onExport(map);
    };
    save.setColour(juce::TextButton::buttonColourId, kAccent.darker(0.3f));
    save.onClick = [this]
    {
        if (onSave)
            onSave(map);
        status = "Saved: the keys work now";
        repaint();
    };
    preset.addItem("Preset...", 1);
    preset.addItem(presetName(KeymapPreset::Default), 2);
    preset.addItem(presetName(KeymapPreset::LaptopCompact), 3);
    preset.setSelectedId(1, juce::dontSendNotification);
    preset.onChange = [this]
    {
        if (preset.getSelectedId() == 2 || preset.getSelectedId() == 3)
        {
            const auto p = preset.getSelectedId() == 3 ? KeymapPreset::LaptopCompact : KeymapPreset::Default;
            map = presetKeymap(p);
            status = juce::String("The ") + presetName(p) + " map (SAVE to use it)";
            list.repaint();
            repaint();
        }
        preset.setSelectedId(1, juce::dontSendNotification);
    };
    addAndMakeVisible(preset);
    refilter();
    setSize(720, 560);
}

KeymapEditor::~KeymapEditor() = default;

void KeymapEditor::setBindings(std::vector<KeyBinding> bindings)
{
    map = std::move(bindings);
    list.repaint();
    repaint();
}

void KeymapEditor::refilter()
{
    const auto text = filter.getText().trim().toLowerCase();
    shown.clear();
    for (const auto c : all)
    {
        const auto name = juce::String(commandName(c)).toLowerCase();
        if (text.isEmpty() || name.contains(text) || juce::String(commandKey(c)).toLowerCase().contains(text))
            shown.push_back(c);
    }
    list.updateContent();
    list.repaint();
}

void KeymapEditor::setFilter(const juce::String& text)
{
    filter.setText(text, true);
    refilter();
}

void KeymapEditor::selectCommand(CommandId command)
{
    const auto it = std::find(shown.begin(), shown.end(), command);
    if (it != shown.end())
        list.selectRow(static_cast<int>(it - shown.begin()));
}

std::optional<CommandId> KeymapEditor::selected() const
{
    const int row = list.getSelectedRow();
    if (row < 0 || row >= static_cast<int>(shown.size()))
        return std::nullopt;
    return shown[static_cast<std::size_t>(row)];
}

void KeymapEditor::beginCapture()
{
    if (!selected())
    {
        status = "Pick an action first";
        repaint();
        return;
    }
    capturing = true;
    status = "Press the key for " + juce::String(commandName(*selected())) + " ...";
    repaint();
}

void KeymapEditor::cancelCapture()
{
    capturing = false;
    repaint();
}

void KeymapEditor::captureChord(const KeyChord& chord)
{
    if (!capturing)
        return;
    capturing = false;
    const auto c = selected();
    if (!c)
        return;
    auto result = assignKey(std::move(map), *c, chord);
    map = std::move(result.bindings);
    status = juce::String(commandName(*c)) + ": " + describeChord(chord);
    if (!result.displaced.empty()) // the conflict: say who lost the key
        status << "  (taken from " << commandName(result.displaced.front()) << ", which has no key there now)";
    list.repaint();
    repaint();
}

void KeymapEditor::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selectedRow)
{
    if (row < 0 || row >= static_cast<int>(shown.size()))
        return;
    const auto c = shown[static_cast<std::size_t>(row)];
    if (selectedRow)
        g.fillAll(kAccent.withAlpha(0.35f));
    g.setColour(kText);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(commandName(c), 6, 0, width * 2 / 3 - 8, height, juce::Justification::centredLeft, true);
    const auto keys = keysOf(map, c);
    g.setColour(keys.empty() ? kDim : kText);
    g.drawText(keys.empty() ? "-" : keys, width * 2 / 3, 0, width / 3 - 6, height, juce::Justification::centredRight, true);
}

void KeymapEditor::selectedRowsChanged(int)
{
    capturing = false;
    if (const auto c = selected())
        status = juce::String(commandName(*c)) + ": " + (keysOf(map, *c).empty() ? std::string("no key") : keysOf(map, *c));
    repaint();
}

void KeymapEditor::resized()
{
    auto area = getLocalBounds().reduced(10);
    filter.setBounds(area.removeFromTop(26));
    area.removeFromTop(6);
    auto buttons = area.removeFromBottom(30);
    area.removeFromBottom(24); // the message line
    list.setBounds(area);
    const int w = buttons.getWidth() / 7;
    for (auto* c : {static_cast<juce::Component*>(&setKey), static_cast<juce::Component*>(&clear),
                    static_cast<juce::Component*>(&reset), static_cast<juce::Component*>(&preset),
                    static_cast<juce::Component*>(&importButton), static_cast<juce::Component*>(&exportButton),
                    static_cast<juce::Component*>(&save)})
        c->setBounds(buttons.removeFromLeft(w).reduced(2));
}

void KeymapEditor::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    g.setColour(capturing ? kAccent : kDim);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(status, getLocalBounds().reduced(10).removeFromBottom(54).removeFromTop(22),
               juce::Justification::centredLeft, true);
}

} // namespace devildj::ui
