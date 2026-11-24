#pragma once

// Help for the code editor (P5-16): the reference table (src/codebeat/help/reference.json, embedded) for hover help
// and completion, the starter examples (src/codebeat/help/examples/*.js, embedded), and the completion logic.

#include <juce_core/juce_core.h>

#include <vector>

namespace devildj::ui
{

struct HelpEntry
{
    juce::String name, kind, signature, help, example; // kind: control function method signal program hook
};

const std::vector<HelpEntry>& codeReference();
const HelpEntry* findHelp(const juce::String& name);

struct CodeExample
{
    juce::String title; // its first line's comment, before " - "
    juce::String code;
};
const std::vector<CodeExample>& codeExamples(); // in file order (01 ... 15)

// What Ctrl+Space offers at `caret` in `text`: inside a string the sound names (and the scale names after ":"),
// after "." the methods and controls, elsewhere the functions, controls, signals, program functions and hooks.
struct Completion
{
    int start = 0;           // the word being typed: [start, caret)
    juce::String prefix;
    bool inString = false;
    std::vector<juce::String> options; // sorted; those starting with the prefix (case first matches first)
};
Completion completionsAt(const juce::String& text, int caret, const std::vector<juce::String>& soundNames);

// The identifier at character `index` (empty if none), and where it starts.
juce::String wordAt(const juce::String& text, int index, int* start = nullptr);

} // namespace devildj::ui
