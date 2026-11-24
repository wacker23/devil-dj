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

    ~Page()
    {
        document.removeListener(&listener);
        button.removeMouseListener(&mouse);
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
}

CodeEditorPanel::~CodeEditorPanel() = default;

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
    for (auto& p : pages)
        p->editor->setBounds(area);
}

void CodeEditorPanel::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    g.setColour(kPanel);
    g.fillRect(getLocalBounds().removeFromTop(kTabHeight));
}

} // namespace devildj::ui
