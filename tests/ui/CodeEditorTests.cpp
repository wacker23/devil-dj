// P5-13: the code editor - Code Beat colours, tabs, and keys while typing in it.

#include "ui/CodeEditorPanel.h"
#include "ui/CodeTokeniser.h"
#include "ui/KeyboardController.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using devildj::ui::CodeEditorPanel;
using devildj::ui::CodeTokeniser;

namespace
{
// The tokens of a text: "type:text" for each (spaces dropped).
std::vector<std::string> tokens(const juce::String& text)
{
    juce::CodeDocument doc;
    doc.replaceAllContent(text);
    CodeTokeniser t;
    juce::CodeDocument::Iterator it(doc);
    std::vector<std::string> out;
    const char* names[] = {"err", "comment", "kw", "id", "api", "num", "quote", "op", "br", "punct",
                           "word", "mnum", "rest", "mop"};
    while (!it.isEOF())
    {
        const int start = it.getPosition();
        const int type = t.readNextToken(it);
        const int end = it.getPosition();
        if (end == start)
            break; // no progress: the end
        const auto piece = doc.getTextBetween(juce::CodeDocument::Position(doc, start), juce::CodeDocument::Position(doc, end)).trim();
        if (piece.isNotEmpty())
            out.push_back(std::string(names[type]) + ":" + piece.toStdString());
    }
    return out;
}
} // namespace

TEST_CASE("Code editor colours: JavaScript outside strings, mini-notation inside", "[ui][codebeat][editor]")
{
    CHECK(tokens(R"(s("bd [~ sd]*2").gain(0.5))") ==
          std::vector<std::string>{"api:s", "br:(", "quote:\"", "word:bd", "mop:[", "rest:~", "word:sd", "mop:]",
                                   "mop:*", "mnum:2", "quote:\"", "br:)", "punct:.", "api:gain", "br:(", "num:0.5",
                                   "br:)"});
    CHECK(tokens("const x = note('c3 e3:2 -1') // the end") ==
          std::vector<std::string>{"kw:const", "id:x", "op:=", "api:note", "br:(", "quote:'", "word:c3", "word:e3",
                                   "mop::", "mnum:2", "mnum:-1", "quote:'", "br:)", "comment:// the end"});
    CHECK(tokens("/* a \"not a string\" */ stack(s(`hh*8`))") ==
          std::vector<std::string>{"comment:/* a \"not a string\" */", "api:stack", "br:(", "api:s", "br:(",
                                   "quote:`", "word:hh", "mop:*", "mnum:8", "quote:`", "br:)", "br:)"});
    // An escaped quote doesn't end the string.
    CHECK(tokens(R"(s("a\"b"))") ==
          std::vector<std::string>{"api:s", "br:(", "quote:\"", "word:a", "mop:\\\"", "word:b", "quote:\"", "br:)"});
    // Each line on its own: a line inside a multi-line program colours the same as alone.
    CHECK(tokens("stack(\n  s(\"bd*4\"),\n)") ==
          std::vector<std::string>{"api:stack", "br:(", "api:s", "br:(", "quote:\"", "word:bd", "mop:*", "mnum:4",
                                   "quote:\"", "br:)", "punct:,", "br:)"});
    CHECK(CodeTokeniser::isApiName("every"));
    CHECK_FALSE(CodeTokeniser::isApiName("banana"));
    CHECK(CodeTokeniser().getDefaultColourScheme().types.size() == CodeTokeniser::Count);
}

TEST_CASE("Code editor panel: tabs, typing reported per tab, switching wraps", "[ui][codebeat][editor]")
{
    CodeEditorPanel panel;
    panel.setSize(800, 400);
    std::vector<std::pair<int, juce::String>> typed;
    std::vector<int> shown;
    panel.onTextChanged = [&](int i, const juce::String& t) { typed.emplace_back(i, t); };
    panel.onShown = [&](int i) { shown.push_back(i); };
    panel.setTabs({{"one", "s(\"bd\")"}, {"two", "s(\"hh\")"}}, 1);
    CHECK(panel.count() == 2);
    CHECK(panel.current() == 1);
    CHECK(typed.empty()); // loading isn't typing
    CHECK(panel.tabButton(1)->getToggleState());
    CHECK_FALSE(panel.tabButton(0)->getToggleState());
    CHECK(panel.editor(1)->isVisible());
    CHECK_FALSE(panel.editor(0)->isVisible());

    panel.document(0)->insertText(0, "// first\n");
    REQUIRE(typed.size() == 1);
    CHECK(typed[0].first == 0); // the right tab, even though another is shown
    CHECK(typed[0].second == "// first\ns(\"bd\")");
    // Each tab keeps its own undo.
    panel.document(0)->undo();
    CHECK(panel.text(0) == "s(\"bd\")");
    CHECK(panel.text(1) == "s(\"hh\")");

    panel.nextTab(1);
    CHECK(panel.current() == 0); // wraps
    panel.nextTab(-1);
    CHECK(panel.current() == 1);
    panel.addTab({"three", ""});
    CHECK(panel.current() == 2);
    panel.renameTab(2, "drums");
    CHECK(panel.name(2) == "drums");
    CHECK(panel.tabButton(2)->getButtonText() == "drums");
    panel.removeTab(0);
    CHECK(panel.count() == 2);
    CHECK(panel.current() == 1);
    CHECK(panel.name(0) == "two");
    // Clicking a tab after a removal shows the one clicked (indices are re-linked).
    panel.tabButton(0)->onClick();
    CHECK(panel.current() == 0);
    CHECK_FALSE(shown.empty());
}

TEST_CASE("Keys while typing in the code editor: only the code deck's, never the decks'", "[ui][codebeat][keys]")
{
    using namespace devildj::ui;
    ActionDispatcher dispatcher;
    std::vector<CommandId> performed;
    for (auto id : allCommands())
        dispatcher.setHandler(id, [&performed, id](ActionPhase p)
                              {
                                  if (p == ActionPhase::Press)
                                      performed.push_back(id);
                              });
    KeyboardController keys(dispatcher);
    const auto press = [&](std::uint16_t code, std::uint8_t m, KeyContext ctx, bool typing)
    {
        keys.handle({code, true, false, m}, ctx, typing);
        keys.handle({code, false, false, m}, ctx, typing);
    };
    // Typing in the code editor: Ctrl+Enter evaluates, Ctrl+Shift+Enter evaluates now, Ctrl+. hushes,
    // Ctrl+PageDown switches buffers, Ctrl+Tab leaves the panel...
    press(scan::Enter, mods::Ctrl, KeyContext::CodeBeat, true);
    press(scan::Enter, mods::Ctrl | mods::Shift, KeyContext::CodeBeat, true);
    press(scan::Period, mods::Ctrl, KeyContext::CodeBeat, true);
    press(scan::PageDown, mods::Ctrl, KeyContext::CodeBeat, true);
    press(scan::Tab, mods::Ctrl, KeyContext::CodeBeat, true);
    CHECK(performed == std::vector<CommandId>{globalCommand(GlobalAction::CodeEvaluate),
                                              globalCommand(GlobalAction::CodeEvaluateNow),
                                              globalCommand(GlobalAction::CodeHush),
                                              globalCommand(GlobalAction::CodeNextBuffer),
                                              globalCommand(GlobalAction::SwitchPanel)});
    // ...but plain keys are text: deck A's CUE (Z) must not fire, nor deck B's LOAD (Enter).
    performed.clear();
    press(scan::Z, mods::None, KeyContext::CodeBeat, true);
    press(scan::Enter, mods::None, KeyContext::CodeBeat, true);
    CHECK(performed.empty());
    // In another text field (the library search) nothing is a shortcut, as before.
    press(scan::Enter, mods::Ctrl, KeyContext::Main, true);
    CHECK(performed.empty());
    // Outside text fields the decks' keys work as ever.
    press(scan::Z, mods::None, KeyContext::Main, false);
    CHECK(performed == std::vector<CommandId>{deckCommand(0, DeckAction::Cue)});
}

TEST_CASE("Code editor panel: an error marked on its line, notes and the status line (P5-14)", "[ui][codebeat][editor]")
{
    CodeEditorPanel panel;
    panel.setSize(800, 400);
    panel.setTabs({{"one", "const a = 1;\nlet b = ;\ns(\"bd\")\n"}, {"two", "s(\"hh\")"}}, 0);
    CHECK_FALSE(panel.hasError());
    CHECK(panel.markerBounds().isEmpty());

    panel.setError(0, 2, 9, "SyntaxError: unexpected token");
    CHECK(panel.hasError());
    CHECK(panel.barText() == "line 2:9  SyntaxError: unexpected token");
    // The marker sits on line 2 of the editor (below line 1, inside the editor's area).
    const auto m = panel.markerBounds();
    REQUIRE_FALSE(m.isEmpty());
    const auto* ed = panel.editor(0);
    const auto line1 = panel.getLocalArea(ed, ed->getCharacterBounds(juce::CodeDocument::Position(*panel.document(0), 0, 0)));
    const auto line2 = panel.getLocalArea(ed, ed->getCharacterBounds(juce::CodeDocument::Position(*panel.document(0), 1, 0)));
    CHECK(m.getY() == line2.getY());
    CHECK(m.getY() > line1.getY());
    CHECK(ed->getBounds().contains(m));
    // Only on its own tab.
    panel.showTab(1);
    CHECK(panel.markerBounds().isEmpty());
    panel.showTab(0);
    CHECK_FALSE(panel.markerBounds().isEmpty());
    // A place outside the text is kept inside it; no place at all is just the message.
    panel.setError(0, 99, 0, "too far");
    CHECK_FALSE(panel.markerBounds().isEmpty());
    panel.setError(0, 0, 0, "Error: no place");
    CHECK(panel.barText() == "Error: no place");
    CHECK(panel.markerBounds().isEmpty());

    // A good evaluation clears it; notes and the status line.
    panel.clearError();
    CHECK_FALSE(panel.hasError());
    CHECK(panel.markerBounds().isEmpty());
    panel.setNote("plays from the next cycle", false);
    CHECK(panel.barText() == "plays from the next cycle");
    panel.setStatus("cycle 3.5   0.50 cps   8 events/s   0 late");
    CHECK(panel.statusText() == "cycle 3.5   0.50 cps   8 events/s   0 late");
}
