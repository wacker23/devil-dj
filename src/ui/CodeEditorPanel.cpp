#include "ui/CodeEditorPanel.h"

#include <algorithm>

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff121217);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kText(0xffe8e8ee);
const juce::Colour kDim(0xff8a8a96);
const juce::Colour kCode(0xff5fd38a);
constexpr int kTabHeight = 26;
constexpr int kBarHeight = 22;
const juce::Colour kError(0xffe05050);
const juce::Colour kWarning(0xffe0a030);

void styleTab(juce::TextButton& b)
{
    b.setColour(juce::TextButton::buttonColourId, kPanel);
    b.setColour(juce::TextButton::buttonOnColourId, kCode);
    b.setColour(juce::TextButton::textColourOffId, kText);
    b.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    b.setWantsKeyboardFocus(false);
}
} // namespace

// One tab: its document (with a listener that knows it), its editor, its button (with the menu's mouse listener).
struct CodeEditorPanel::Page
{
    struct DocumentListener final : juce::CodeDocument::Listener
    {
        std::function<void()> onChange;
        void codeDocumentTextInserted(const juce::String&, int) override { onChange(); }
        void codeDocumentTextDeleted(int, int) override { onChange(); }
    };
    struct TabMouse final : juce::MouseListener
    {
        std::function<void()> onMenu;
        void mouseDown(const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() && onMenu)
                onMenu();
        }
        void mouseDoubleClick(const juce::MouseEvent&) override
        {
            if (onMenu)
                onMenu();
        }
    };

    juce::String name;
    juce::CodeDocument document;
    DocumentListener listener;
    std::unique_ptr<juce::CodeEditorComponent> editor;
    TabMouse mouse;
    juce::TextButton button; // after `mouse`: destroyed first, while the listener still exists

    juce::MouseListener* hover = nullptr; // the panel, listening for hover help

    ~Page()
    {
        document.removeListener(&listener);
        button.removeMouseListener(&mouse);
        if (editor != nullptr && hover != nullptr)
            editor->removeMouseListener(hover);
    }
};

CodeEditorPanel::CodeEditorPanel()
{
    styleTab(plus);
    plus.setTooltip("A new code buffer");
    plus.onClick = [this]
    {
        if (onAdd)
            onAdd();
    };
    addAndMakeVisible(plus);
    marker.setInterceptsMouseClicks(false, false);
    addChildComponent(marker);
    lights.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(lights);
    styleTab(examples);
    examples.setTooltip("Open one of the starter patterns in a new tab");
    examples.onClick = [this]
    {
        juce::PopupMenu m;
        const auto& all = codeExamples();
        for (std::size_t i = 0; i < all.size(); ++i)
            m.addItem(juce::String(static_cast<int>(i) + 1) + ". " + all[i].title, [this, i]
                      {
                          if (onExample)
                              onExample(codeExamples()[i].title, codeExamples()[i].code);
                      });
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&examples));
    };
    addAndMakeVisible(examples);
    styleTab(render);
    render.setTooltip("Render the shown tab's code offline: to a WAV file, or a loop in the sampler");
    render.onClick = [this]
    {
        if (onRender)
            onRender();
    };
    addAndMakeVisible(render);
    bubble.setInterceptsMouseClicks(false, false);
    addChildComponent(bubble);
}

void CodeEditorPanel::Bubble::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(juce::Colour(0xf0262630));
    g.fillRoundedRectangle(r, 5.0f);
    g.setColour(kCode.withAlpha(0.7f));
    g.drawRoundedRectangle(r, 5.0f, 1.0f);
    auto area = getLocalBounds().reduced(8, 6);
    g.setColour(kCode);
    g.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::bold));
    g.drawText(title, area.removeFromTop(18), juce::Justification::centredLeft, true);
    g.setColour(kText);
    g.setFont(juce::FontOptions(13.0f));
    g.drawFittedText(text, area, juce::Justification::topLeft, 8, 1.0f);
}

void CodeEditorPanel::setSoundNames(std::vector<juce::String> names)
{
    soundNames = std::move(names);
}

bool CodeEditorPanel::completeWith(const juce::String& option)
{
    auto* ed = editor(shown);
    auto* doc = document(shown);
    if (ed == nullptr || doc == nullptr)
        return false;
    const int caret = ed->getCaretPos().getPosition();
    const auto c = completionsAt(doc->getAllContent(), caret, soundNames);
    doc->replaceSection(c.start, caret, option);
    ed->moveCaretTo(juce::CodeDocument::Position(*doc, c.start + option.length()), false);
    return true;
}

void CodeEditorPanel::showCompletions()
{
    auto* ed = editor(shown);
    auto* doc = document(shown);
    if (ed == nullptr || doc == nullptr)
        return;
    const auto caretPos = ed->getCaretPos();
    const auto c = completionsAt(doc->getAllContent(), caretPos.getPosition(), soundNames);
    if (c.options.empty())
    {
        setNote(c.inString ? "no sound starts with \"" + c.prefix + "\"" : "nothing to complete", true);
        return;
    }
    if (c.options.size() == 1)
    {
        completeWith(c.options.front());
        return;
    }
    juce::PopupMenu m;
    const std::size_t shownCount = std::min<std::size_t>(c.options.size(), 40);
    for (std::size_t i = 0; i < shownCount; ++i)
    {
        const auto option = c.options[i];
        const auto* help = c.inString ? nullptr : findHelp(option);
        m.addItem(help != nullptr ? option + "    " + help->signature : option, [this, option]
                  {
                      completeWith(option);
                      focusEditor();
                  });
    }
    if (c.options.size() > shownCount)
        m.addItem("... " + juce::String(static_cast<int>(c.options.size() - shownCount)) + " more: type more letters", false, false, [] {});
    const auto at = ed->localAreaToGlobal(ed->getCharacterBounds(caretPos));
    m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(at).withMaximumNumColumns(1));
}

void CodeEditorPanel::showHelpFor(const juce::String& name, juce::Point<int> where)
{
    const auto* help = findHelp(name);
    if (help == nullptr)
    {
        hideHelp();
        return;
    }
    bubble.title = help->signature;
    bubble.text = help->help + (help->example.isNotEmpty() ? "\n\ne.g.  " + help->example : juce::String());
    const int width = std::min(460, std::max(240, getWidth() - 20));
    juce::AttributedString as;
    as.append(bubble.text, juce::FontOptions(13.0f));
    juce::TextLayout layout;
    layout.createLayout(as, static_cast<float>(width - 16));
    const int height = static_cast<int>(std::ceil(layout.getHeight())) + 18 + 16;
    auto box = juce::Rectangle<int>(where.x + 12, where.y + 18, width, height);
    if (box.getRight() > getWidth())
        box.setX(std::max(0, getWidth() - width));
    if (box.getBottom() > getHeight())
        box.setY(std::max(0, where.y - height - 8));
    bubble.setBounds(box);
    bubble.setVisible(true);
    bubble.toFront(false);
    bubble.repaint();
}

void CodeEditorPanel::hideHelp()
{
    stopTimer();
    hoverWord.clear();
    bubble.setVisible(false);
}

void CodeEditorPanel::mouseMove(const juce::MouseEvent& e)
{
    auto* ed = editor(shown);
    if (ed == nullptr || !(e.eventComponent == ed || ed->isParentOf(e.eventComponent)))
        return;
    const auto local = e.getEventRelativeTo(ed).getPosition();
    const auto pos = ed->getPositionAt(local.x, local.y);
    const auto word = wordAt(document(shown)->getAllContent(), pos.getPosition());
    hoverAt = e.getEventRelativeTo(this).getPosition();
    if (word == hoverWord)
        return;
    hoverWord = word;
    bubble.setVisible(false);
    if (findHelp(word) != nullptr)
        startTimer(450); // a short rest on the word, then its help
    else
        stopTimer();
}

void CodeEditorPanel::mouseExit(const juce::MouseEvent& e)
{
    if (auto* ed = editor(shown); ed != nullptr && (e.eventComponent == ed || ed->isParentOf(e.eventComponent)))
        hideHelp();
}

void CodeEditorPanel::mouseDown(const juce::MouseEvent&)
{
    hideHelp();
}

void CodeEditorPanel::timerCallback()
{
    stopTimer();
    if (hoverWord.isNotEmpty())
        showHelpFor(hoverWord, hoverAt);
}

void CodeEditorPanel::Lights::paint(juce::Graphics& g)
{
    for (const auto& r : rects)
    {
        g.setColour(kCode.withAlpha(0.28f));
        g.fillRoundedRectangle(r.toFloat().expanded(1.0f, 0.0f), 3.0f);
        g.setColour(kCode.withAlpha(0.9f));
        g.fillRect(r.getX(), r.getBottom() - 2, r.getWidth(), 2);
    }
}

void CodeEditorPanel::setHighlights(int tab, std::vector<std::pair<int, int>> ranges)
{
    if (tab == litTab && ranges == litRanges)
        return;
    litTab = tab;
    litRanges = std::move(ranges);
    placeLights();
}

std::vector<juce::Rectangle<int>> CodeEditorPanel::highlightRects() const
{
    std::vector<juce::Rectangle<int>> out;
    for (const auto& r : lights.rects)
        out.push_back(r + lights.getPosition());
    return out;
}

void CodeEditorPanel::placeLights()
{
    lights.rects.clear();
    auto* ed = editor(litTab);
    if (litTab == shown && ed != nullptr && ed->isVisible())
    {
        lights.setBounds(ed->getBounds().withTrimmedRight(12));
        auto& doc = *document(litTab);
        const int length = doc.getNumCharacters();
        for (const auto& [start, end] : litRanges)
        {
            // Line by line: from the range's first character to its last on each line it covers.
            const juce::CodeDocument::Position a(doc, std::clamp(start, 0, length));
            const juce::CodeDocument::Position b(doc, std::clamp(end, 0, length));
            for (int line = a.getLineNumber(); line <= b.getLineNumber(); ++line)
            {
                const int from = line == a.getLineNumber() ? a.getIndexInLine() : 0;
                const int to = line == b.getLineNumber() ? b.getIndexInLine()
                                                         : doc.getLine(line).trimCharactersAtEnd("\r\n").length();
                if (to <= from)
                    continue;
                const auto first = ed->getCharacterBounds(juce::CodeDocument::Position(doc, line, from));
                const auto lastChar = ed->getCharacterBounds(juce::CodeDocument::Position(doc, line, to - 1));
                const auto rect = lights.getLocalArea(ed, first.getUnion(lastChar));
                if (rect.intersects(lights.getLocalBounds()))
                    lights.rects.push_back(rect.getIntersection(lights.getLocalBounds()));
            }
        }
    }
    lights.toFront(false);
    lights.repaint();
}

CodeEditorPanel::~CodeEditorPanel() = default;

void CodeEditorPanel::Marker::paint(juce::Graphics& g)
{
    g.setColour(kError.withAlpha(0.18f));
    g.fillRect(getLocalBounds());
    g.setColour(kError);
    g.fillRect(getLocalBounds().removeFromLeft(3));
    g.drawLine(0.0f, static_cast<float>(getHeight()) - 1.0f, static_cast<float>(getWidth()), static_cast<float>(getHeight()) - 1.0f, 1.0f);
}

void CodeEditorPanel::setError(int tab, int line, int column, const juce::String& message)
{
    error = {true, tab, line, column, message};
    refreshMarker();
    repaint();
}

void CodeEditorPanel::clearError()
{
    error = {};
    marker.setVisible(false);
    repaint();
}

void CodeEditorPanel::setNote(const juce::String& text, bool warning)
{
    note = text;
    noteIsWarning = warning;
    repaint();
}

void CodeEditorPanel::setStatus(const juce::String& text)
{
    if (text == status)
        return;
    status = text;
    repaint(getLocalBounds().removeFromBottom(kBarHeight));
}

juce::String CodeEditorPanel::barText() const
{
    if (error.active)
        return (error.line > 0 ? "line " + juce::String(error.line) + (error.column > 0 ? ":" + juce::String(error.column) : juce::String()) + "  "
                               : juce::String()) + error.message;
    return note;
}

juce::Rectangle<int> CodeEditorPanel::markerBounds() const
{
    return marker.isVisible() ? marker.getBounds() : juce::Rectangle<int>();
}

void CodeEditorPanel::refreshMarker()
{
    auto* ed = editor(error.tab);
    if (!error.active || error.line <= 0 || error.tab != shown || ed == nullptr || !ed->isVisible())
    {
        marker.setVisible(false);
        return;
    }
    auto& doc = *document(error.tab);
    const int line = std::min(error.line, std::max(1, doc.getNumLines())) - 1;
    const auto start = ed->getCharacterBounds(juce::CodeDocument::Position(doc, line, 0));
    const auto area = ed->getBounds();
    // From the line's first character (after the line numbers) to the editor's right edge (less the scrollbar).
    auto row = getLocalArea(ed, start).withRight(area.getRight() - 12);
    if (row.getBottom() <= area.getY() || row.getY() >= area.getBottom())
    {
        marker.setVisible(false); // scrolled out of view
        return;
    }
    marker.setBounds(row.getIntersection(area));
    marker.setVisible(true);
    marker.toFront(false);
}

std::unique_ptr<CodeEditorPanel::Page> CodeEditorPanel::makePage(const Tab& tab)
{
    auto p = std::make_unique<Page>();
    p->name = tab.name;
    p->document.setNewLineCharacters("\n");
    p->document.replaceAllContent(tab.text);
    p->document.clearUndoHistory();
    p->document.setSavePoint();
    const auto* doc = &p->document;
    p->listener.onChange = [this, doc] { changed(*doc); };
    p->document.addListener(&p->listener); // after loading: the load itself isn't a change
    p->editor = std::make_unique<juce::CodeEditorComponent>(p->document, &tokeniser);
    auto& ed = *p->editor;
    ed.setLineNumbersShown(true);
    ed.setTabSize(2, true);
    ed.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 15.0f, juce::Font::plain));
    ed.setColourScheme(tokeniser.getDefaultColourScheme());
    ed.setColour(juce::CodeEditorComponent::backgroundColourId, kBackground);
    ed.setColour(juce::CodeEditorComponent::defaultTextColourId, kText);
    ed.setColour(juce::CodeEditorComponent::lineNumberBackgroundId, kPanel);
    ed.setColour(juce::CodeEditorComponent::lineNumberTextId, kDim);
    ed.setColour(juce::CodeEditorComponent::highlightColourId, kCode.withAlpha(0.3f));
    ed.setColour(juce::CaretComponent::caretColourId, kCode);
    ed.setScrollbarThickness(10);
    addChildComponent(ed);
    ed.addMouseListener(this, true); // hover help (P5-16)
    p->hover = this;
    p->button.setButtonText(tab.name);
    styleTab(p->button);
    p->button.setTooltip("Right-click (or double-click) to rename or close");
    p->button.addMouseListener(&p->mouse, false);
    addAndMakeVisible(p->button);
    return p;
}

void CodeEditorPanel::relink()
{
    for (int i = 0; i < count(); ++i)
    {
        auto& p = *pages[static_cast<std::size_t>(i)];
        p.button.onClick = [this, i]
        {
            showTab(i);
            focusEditor();
        };
        p.mouse.onMenu = [this, i] { showTabMenu(i); };
    }
    resized();
}

void CodeEditorPanel::setTabs(const std::vector<Tab>& tabs, int current)
{
    pages.clear();
    for (const auto& t : tabs)
        pages.push_back(makePage(t));
    relink();
    showTab(std::clamp(current, 0, std::max(0, count() - 1)));
}

void CodeEditorPanel::addTab(const Tab& tab)
{
    pages.push_back(makePage(tab));
    relink();
    showTab(count() - 1);
    focusEditor();
}

void CodeEditorPanel::renameTab(int index, const juce::String& newName)
{
    if (index < 0 || index >= count())
        return;
    auto& p = *pages[static_cast<std::size_t>(index)];
    p.name = newName;
    p.button.setButtonText(newName);
    resized();
}

void CodeEditorPanel::removeTab(int index)
{
    if (index < 0 || index >= count())
        return;
    pages.erase(pages.begin() + index);
    relink();
    showTab(std::clamp(shown > index ? shown - 1 : shown, 0, std::max(0, count() - 1)));
}

juce::String CodeEditorPanel::name(int index) const
{
    return index >= 0 && index < count() ? pages[static_cast<std::size_t>(index)]->name : juce::String();
}

juce::String CodeEditorPanel::text(int index) const
{
    return index >= 0 && index < count() ? pages[static_cast<std::size_t>(index)]->document.getAllContent() : juce::String();
}

juce::CodeDocument* CodeEditorPanel::document(int index)
{
    return index >= 0 && index < count() ? &pages[static_cast<std::size_t>(index)]->document : nullptr;
}

juce::CodeEditorComponent* CodeEditorPanel::editor(int index)
{
    return index >= 0 && index < count() ? pages[static_cast<std::size_t>(index)]->editor.get() : nullptr;
}

juce::TextButton* CodeEditorPanel::tabButton(int index)
{
    return index >= 0 && index < count() ? &pages[static_cast<std::size_t>(index)]->button : nullptr;
}

void CodeEditorPanel::showTab(int index)
{
    if (count() == 0)
        return;
    const bool hadFocus = editorHasFocus();
    shown = std::clamp(index, 0, count() - 1);
    for (int t = 0; t < count(); ++t)
    {
        auto& p = *pages[static_cast<std::size_t>(t)];
        p.editor->setVisible(t == shown);
        p.button.setToggleState(t == shown, juce::dontSendNotification);
    }
    if (hadFocus)
        focusEditor();
    refreshMarker(); // an error is marked on its own tab only
    placeLights();
    if (onShown)
        onShown(shown);
}

void CodeEditorPanel::nextTab(int delta)
{
    if (count() > 0)
        showTab(((shown + delta) % count() + count()) % count());
}

void CodeEditorPanel::focusEditor()
{
    if (auto* ed = editor(shown); ed != nullptr && ed->isShowing())
        ed->grabKeyboardFocus();
}

bool CodeEditorPanel::editorHasFocus() const
{
    return std::any_of(pages.begin(), pages.end(), [](const auto& p) { return p->editor->hasKeyboardFocus(true); });
}

void CodeEditorPanel::changed(const juce::CodeDocument& doc)
{
    for (int i = 0; i < count(); ++i)
        if (&pages[static_cast<std::size_t>(i)]->document == &doc)
        {
            if (onTextChanged)
                onTextChanged(i, doc.getAllContent());
            return;
        }
}

void CodeEditorPanel::showTabMenu(int index)
{
    if (index < 0 || index >= count())
        return;
    juce::PopupMenu m;
    m.addItem("Rename...", [this, index] { askRename(index); });
    m.addItem("Close (its file goes to Code\\Closed)", count() > 1, false, [this, index]
              {
                  if (onClose)
                      onClose(index);
              });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(tabButton(index)));
}

void CodeEditorPanel::askRename(int index)
{
    auto* w = new juce::AlertWindow("Rename the buffer", "Its new name:", juce::MessageBoxIconType::NoIcon, this);
    w->addTextEditor("name", name(index));
    w->addButton("RENAME", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("CANCEL", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<CodeEditorPanel> safe(this);
    w->enterModalState(true, juce::ModalCallbackFunction::create([safe, w, index](int result)
                                                                 {
                                                                     if (result == 1 && safe != nullptr && safe->onRename)
                                                                         safe->onRename(index, w->getTextEditorContents("name"));
                                                                 }),
                       true);
}

void CodeEditorPanel::resized()
{
    auto area = getLocalBounds();
    auto bar = area.removeFromTop(kTabHeight).reduced(2, 2);
    const juce::Font font(juce::FontOptions(14.0f));
    for (auto& p : pages)
    {
        const int w = std::clamp(juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, p->name)) + 24, 60, 200);
        p->button.setBounds(bar.removeFromLeft(w));
        bar.removeFromLeft(2);
    }
    plus.setBounds(bar.removeFromLeft(28));
    examples.setBounds(bar.removeFromRight(90));
    bar.removeFromRight(2);
    render.setBounds(bar.removeFromRight(80));
    area.removeFromBottom(kBarHeight); // the message and the status
    for (auto& p : pages)
        p->editor->setBounds(area);
    refreshMarker();
    placeLights();
}

void CodeEditorPanel::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    g.setColour(kPanel);
    g.fillRect(getLocalBounds().removeFromTop(kTabHeight));
    // The bar under the editor: the error / note at the left, the status at the right.
    auto bar = getLocalBounds().removeFromBottom(kBarHeight);
    g.fillRect(bar);
    bar.reduce(8, 0);
    g.setFont(juce::FontOptions(13.0f));
    g.setColour(kDim);
    const int statusWidth = std::min(bar.getWidth() / 2, juce::roundToInt(juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), status)) + 8);
    g.drawText(status, bar.removeFromRight(statusWidth), juce::Justification::centredRight, true);
    g.setColour(error.active ? kError : noteIsWarning ? kWarning : kText);
    g.drawText(barText(), bar, juce::Justification::centredLeft, true);
}

} // namespace devildj::ui
