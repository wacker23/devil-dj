#pragma once

// The code deck's editor (P5-13): tabs of code buffers above a code editor with line numbers and Code Beat
// colours (ui::CodeTokeniser). Each tab keeps its own document, so undo stays with its buffer.
//
// The panel shows what the app gives it (setTabs / addTab / renameTab / removeTab) and reports what the owner does:
// typing (onTextChanged, on every change), switching (onShown), "+" (onAdd), rename / close from a tab's menu
// (right-click or double-click: onRename / onClose). The app keeps the buffers on disk (app::CodeBuffers).
// Ctrl+PageUp / Ctrl+PageDown switch tabs (through the app's key handling: nextTab).

#include "ui/CodeTokeniser.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>
#include <vector>

namespace devildj::ui
{

class CodeEditorPanel final : public juce::Component
{
public:
    CodeEditorPanel();
    ~CodeEditorPanel() override;

    struct Tab
    {
        juce::String name, text;
    };
    void setTabs(const std::vector<Tab>& tabs, int current);
    void addTab(const Tab& tab); // shown at once
    void renameTab(int index, const juce::String& name);
    void removeTab(int index);

    int count() const noexcept { return static_cast<int>(pages.size()); }
    int current() const noexcept { return shown; }
    juce::String name(int index) const;
    juce::String text(int index) const;
    juce::CodeDocument* document(int index);
    juce::CodeEditorComponent* editor(int index);
    juce::TextButton* tabButton(int index);

    void showTab(int index);
    void nextTab(int delta); // wraps
    void focusEditor();
    bool editorHasFocus() const;

    std::function<void(int index, const juce::String& text)> onTextChanged;
    std::function<void(int index)> onShown;
    std::function<void()> onAdd;
    std::function<void(int index, const juce::String& newName)> onRename;
    std::function<void(int index)> onClose;

    void showTabMenu(int index);
    void askRename(int index);

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    struct Page;
    std::unique_ptr<Page> makePage(const Tab& tab);
    void changed(const juce::CodeDocument& document);
    void relink(); // tab buttons and menus know their (new) index

    CodeTokeniser tokeniser;
    std::vector<std::unique_ptr<Page>> pages;
    int shown = 0;
    juce::TextButton plus{"+"};
};

} // namespace devildj::ui
