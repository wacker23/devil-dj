#pragma once

#include "ui/Keymaps.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace devildj::ui
{

// The keymap editor (P3-20): every action in a list with its keys (a filter box above it); pick one and SET KEY,
// then press the key - the app passes the next physical key (captureChord); if another action had it, that one
// loses it and the editor says so. CLEAR, DEFAULT (this action's default keys), a preset (Default / Laptop compact),
// IMPORT / EXPORT (JSON files), SAVE (the map becomes the app's keymap). Mouse, touch and the keyboard.
class KeymapEditor final : public juce::Component, private juce::ListBoxModel
{
public:
    explicit KeymapEditor(std::vector<KeyBinding> current);
    ~KeymapEditor() override;

    std::function<void(const std::vector<KeyBinding>&)> onSave;
    std::function<void()> onImport; // the app asks for a file and calls setBindings
    std::function<void(const std::vector<KeyBinding>&)> onExport;

    void setBindings(std::vector<KeyBinding> bindings);
    const std::vector<KeyBinding>& bindings() const noexcept { return map; }

    // Waiting for a key for the selected action (after SET KEY).
    bool isCapturing() const noexcept { return capturing; }
    void beginCapture();
    void captureChord(const KeyChord& chord); // the key pressed (modifier keys alone are ignored by the caller)
    void cancelCapture();

    void selectCommand(CommandId command);
    std::optional<CommandId> selected() const;
    const juce::String& message() const noexcept { return status; }
    int visibleRows() const noexcept { return static_cast<int>(shown.size()); }
    void setFilter(const juce::String& text);

    void resized() override;
    void paint(juce::Graphics& g) override;

    juce::TextButton& setKeyButton() noexcept { return setKey; }
    juce::TextButton& clearButton() noexcept { return clear; }
    juce::TextButton& defaultButton() noexcept { return reset; }
    juce::TextButton& saveButton() noexcept { return save; }
    juce::ComboBox& presetBox() noexcept { return preset; }

private:
    int getNumRows() override { return static_cast<int>(shown.size()); }
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selectedRow) override;
    void selectedRowsChanged(int lastRowSelected) override;
    void refilter();

    std::vector<KeyBinding> map;
    std::vector<CommandId> all, shown;
    juce::TextEditor filter;
    juce::ListBox list{"Actions", this};
    juce::TextButton setKey{"SET KEY"}, clear{"CLEAR"}, reset{"DEFAULT"}, importButton{"IMPORT..."},
        exportButton{"EXPORT..."}, save{"SAVE"};
    juce::ComboBox preset;
    juce::String status;
    bool capturing = false;
};

} // namespace devildj::ui
