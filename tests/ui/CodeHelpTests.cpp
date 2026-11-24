// P5-16: the code editor's help - the reference table, the starter examples, completion and hover help.

#include "app/CodeSoundBank.h"
#include "codebeat/BigStack.h"
#include "codebeat/CodeEvaluator.h"
#include "ui/CodeEditorPanel.h"
#include "ui/CodeHelp.h"
#include "ui/CodeTokeniser.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

using namespace devildj;

namespace
{
// Each code evaluated on its own (a fresh session each), with the sounds its first 8 cycles name.
struct Run
{
    codebeat::EvalResult result;
    std::set<std::string> sounds;
};

std::vector<Run> evaluateEach(const std::vector<juce::String>& codes)
{
    std::vector<Run> runs;
    testing::onBigStack([&]
    {
        for (const auto& code : codes)
        {
            codebeat::CodeEvaluator ev;
            Run r;
            r.result = ev.evaluate(code.toStdString(), true);
            if (r.result.ok)
            {
                const double window[] = {0.0, 1.0, 8.0, 1.0};
                const auto q = ev.runtime().callGlobal("__query", window, 2000.0);
                for (const auto& line : juce::StringArray::fromLines(juce::String::fromUTF8(q.value.c_str())))
                {
                    auto fields = line.fromFirstOccurrenceOf("|", false, false).upToFirstOccurrenceOf("|", false, false);
                    for (const auto& f : juce::StringArray::fromTokens(fields, ";", ""))
                        if (f.startsWith("s="))
                            r.sounds.insert(f.substring(2).toStdString());
                }
            }
            runs.push_back(std::move(r));
        }
    });
    return runs;
}

struct TempFolder
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("devildj-help", "");
    TempFolder() { dir.createDirectory(); }
    ~TempFolder() { dir.deleteRecursively(); }
};
} // namespace

TEST_CASE("Help: the reference table covers every Code Beat name the editor colours", "[ui][codebeat][help]")
{
    const auto& ref = ui::codeReference();
    REQUIRE(ref.size() >= 80);
    std::set<juce::String> names;
    for (const auto& e : ref)
    {
        INFO(e.name);
        CHECK(names.insert(e.name).second); // once each
        CHECK(e.signature.isNotEmpty());
        CHECK(e.help.isNotEmpty());
        CHECK(e.example.isNotEmpty());
        CHECK(std::set<juce::String>{"control", "function", "method", "signal", "program", "hook"}.count(e.kind) == 1);
    }
    for (const auto& name : ui::CodeTokeniser::apiNames())
    {
        INFO(name);
        CHECK(ui::findHelp(name) != nullptr);
    }
    CHECK(ui::findHelp("every")->kind == "method");
    CHECK(ui::findHelp("banana") == nullptr);
}

TEST_CASE("Help: every reference example runs, with sounds that exist", "[ui][codebeat][help]")
{
    std::vector<juce::String> codes;
    for (const auto& e : ui::codeReference())
        codes.push_back(e.example);
    const auto runs = evaluateEach(codes);
    TempFolder t;
    app::CodeSoundBank bank(t.dir);
    for (std::size_t i = 0; i < runs.size(); ++i)
    {
        INFO(ui::codeReference()[i].name << ": " << ui::codeReference()[i].example << " -> " << runs[i].result.error);
        CHECK(runs[i].result.ok);
        for (const auto& s : runs[i].sounds)
        {
            INFO(s);
            CHECK(bank.resolve(s).kind != app::CodeSound::Kind::Unknown);
        }
    }
}

TEST_CASE("Help: the 15 starter examples - titled, they run, they play, their sounds exist", "[ui][codebeat][help]")
{
    const auto& examples = ui::codeExamples();
    REQUIRE(examples.size() == 15);
    std::vector<juce::String> codes;
    for (const auto& e : examples)
    {
        INFO(e.code);
        CHECK(e.title.isNotEmpty());
        CHECK(e.title.length() < 40);
        codes.push_back(e.code);
    }
    CHECK(examples.front().title == "Four on the floor");
    const auto runs = evaluateEach(codes);
    TempFolder t;
    app::CodeSoundBank bank(t.dir);
    for (std::size_t i = 0; i < runs.size(); ++i)
    {
        INFO(examples[i].title << " -> " << runs[i].result.error);
        CHECK(runs[i].result.ok);
        CHECK(runs[i].result.playing);
        CHECK_FALSE(runs[i].sounds.empty());
        for (const auto& s : runs[i].sounds)
        {
            INFO(s);
            CHECK(bank.resolve(s).kind != app::CodeSound::Kind::Unknown);
        }
    }
}

TEST_CASE("Completion: functions, methods after a dot, sounds in strings, scales after a colon", "[ui][codebeat][help]")
{
    const std::vector<juce::String> sounds{"bd", "breaks", "sd", "BassHit"};
    const auto at = [&](const juce::String& textWithBar)
    {
        const int caret = textWithBar.indexOfChar('|');
        return ui::completionsAt(textWithBar.removeCharacters("|"), caret, sounds);
    };
    auto c = at("s(\"b|\")");
    CHECK(c.inString);
    CHECK(c.prefix == "b");
    CHECK(c.options == std::vector<juce::String>{"bd", "breaks", "BassHit"}); // exact-case first
    c = at("s(\"bd ~ sd*2 br|");
    CHECK(c.options == std::vector<juce::String>{"breaks"});
    CHECK(c.start == 13);
    c = at("n(\"0 2\").scale(\"C:mi|\")");
    CHECK(c.options == std::vector<juce::String>{"minor", "minPent", "mixolydian"});
    c = at("st|");
    CHECK_FALSE(c.inString);
    CHECK(c.options == std::vector<juce::String>{"stack"}); // struct is a method: not here
    c = at("s(\"bd\").ev|");
    CHECK(c.options == std::vector<juce::String>{"every"});
    c = at("s(\"bd\").ga|");
    CHECK(c.options == std::vector<juce::String>{"gain"});
    c = at("s(\"bd\").|");
    CHECK(c.options.size() > 40); // everything a pattern can do
    c = at("// s(\"b|");
    CHECK_FALSE(c.inString); // a comment, not a string
    c = at("zzz|");
    CHECK(c.options.empty());
    int start = -1;
    CHECK(ui::wordAt("s(\"bd\").every(2, x)", 10, &start) == "every");
    CHECK(start == 8);
    CHECK(ui::wordAt("a + b", 2) == "");
}

TEST_CASE("Code editor panel: Ctrl+Space with one option completes it; hover help; the examples button", "[ui][codebeat][help]")
{
    ui::CodeEditorPanel panel;
    panel.setSize(900, 500);
    panel.setSoundNames({"bd", "breaks"});
    panel.setTabs({{"one", "s(\"bd\").fa"}}, 0);
    auto& doc = *panel.document(0);
    panel.editor(0)->moveCaretTo(juce::CodeDocument::Position(doc, doc.getNumCharacters()), false);
    panel.showCompletions(); // only "fast" after a dot
    CHECK(panel.text(0) == "s(\"bd\").fast");
    CHECK(panel.editor(0)->getCaretPos().getPosition() == doc.getNumCharacters());
    // In a string: a sound name.
    panel.setTabs({{"one", "s(\"bd br"}}, 0);
    panel.editor(0)->moveCaretTo(juce::CodeDocument::Position(*panel.document(0), 8), false);
    panel.showCompletions();
    CHECK(panel.text(0) == "s(\"bd breaks");
    // Hover help.
    panel.showHelpFor("every", {100, 100});
    CHECK(panel.helpShown());
    CHECK(panel.helpText().contains("every n-th cycle"));
    CHECK(panel.helpText().contains("e.g."));
    panel.showHelpFor("nothing-here", {100, 100});
    CHECK_FALSE(panel.helpShown());
    panel.showHelpFor("sine", {880, 480}); // near the corner: kept inside the panel
    CHECK(panel.helpShown());
    panel.hideHelp();
    CHECK_FALSE(panel.helpShown());
    CHECK(panel.examplesButton().isVisible());
}
