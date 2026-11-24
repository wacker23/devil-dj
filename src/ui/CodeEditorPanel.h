#pragma once

// The code deck's editor (P5-13): tabs of code buffers above a code editor with line numbers and Code Beat
// colours (ui::CodeTokeniser). Each tab keeps its own document, so undo stays with its buffer.
//
// The panel shows what the app gives it (setTabs / addTab / renameTab / removeTab) and reports what the owner does:
// typing (onTextChanged, on every change), switching (onShown), "+" (onAdd), rename / close from a tab's menu
// (right-click or double-click: onRename / onClose). The app keeps the buffers on disk (app::CodeBuffers).
// Ctrl+PageUp / Ctrl+PageDown switch tabs (through the app's key handling: nextTab).

#include "ui/CodeHelp.h"
#include "ui/CodeTokeniser.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>
#include <vector>

namespace devildj::ui
{

class CodeEditorPanel final : public juce::Component, private juce::Timer
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

    // P5-14 feedback. An error: its tab and place (line / column from 1; 0 = unknown) marked on the line, the
    // message in the bar under the editor (it stays until the next good evaluation). A note: the last result or
    // a warning (amber). The status: cycle, cps, events / s, late (right of the bar).
    void setError(int tab, int line, int column, const juce::String& message);
    void clearError();
    void setNote(const juce::String& text, bool warning);
    void setStatus(const juce::String& text);
    void refreshMarker(); // follows scrolling (the app's timer)
    bool hasError() const noexcept { return error.active; }
    juce::String barText() const;                  // what the bar's left side says
    juce::Rectangle<int> markerBounds() const;     // where the marker is (empty when not shown)
    const juce::String& statusText() const noexcept { return status; }

    // P5-15: the code sounding now, lit - character ranges [start, end) in `tab` (from the app's timer).
    void setHighlights(int tab, std::vector<std::pair<int, int>> ranges);

    // P5-16: Ctrl+Space completes the word at the caret (one option: at once; more: a menu); hovering a Code Beat
    // name shows its help; EXAMPLES opens one of the starter patterns (onExample: the app makes it a tab).
    void setSoundNames(std::vector<juce::String> names);
    void showCompletions();
    bool completeWith(const juce::String& option); // replaces the word at the caret (for the menu and tests)
    void showHelpFor(const juce::String& name, juce::Point<int> where); // where: in this component
    void hideHelp();
    bool helpShown() const noexcept { return bubble.isVisible(); }
    juce::String helpText() const { return bubble.text; }
    juce::TextButton& examplesButton() { return examples; }
    std::function<void(const juce::String& title, const juce::String& code)> onExample;
    // P5-17: the RENDER button (the app shows its menu: cycles to a WAV file or the sampler).
    juce::TextButton& renderButton() { return render; }
    std::function<void()> onRender;
    std::vector<juce::Rectangle<int>> highlightRects() const; // where they are drawn (for tests)

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

    struct Error
    {
        bool active = false;
        int tab = 0, line = 0, column = 0;
        juce::String message;
    } error;
    juce::String note, status;
    bool noteIsWarning = false;
    struct Marker final : juce::Component
    {
        void paint(juce::Graphics& g) override;
    } marker;
    struct Lights final : juce::Component
    {
        std::vector<juce::Rectangle<int>> rects; // in this component
        void paint(juce::Graphics& g) override;
    } lights;
    int litTab = 0;
    std::vector<std::pair<int, int>> litRanges;
    void placeLights();

    juce::TextButton examples{"EXAMPLES"}, render{"RENDER"};
    std::vector<juce::String> soundNames;
    struct Bubble final : juce::Component
    {
        juce::String text, title;
        void paint(juce::Graphics& g) override;
    } bubble;
    juce::String hoverWord;
    juce::Point<int> hoverAt;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void timerCallback() override;
};

} // namespace devildj::ui
