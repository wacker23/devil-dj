#pragma once

// Syntax colours for the code deck's editor (P5-13): JavaScript, and inside string literals mini-notation - sound
// names, numbers, rests and its operators each in their own colour, so "bd [~ sd]*2" reads at a glance.
//
// Stateless by design: whether a token is inside a string is found by scanning its line from the start, so any
// line can be re-coloured on its own (the editor re-tokenises lines independently). Strings and block comments
// that run over a line end are coloured line by line.

#include <juce_gui_extra/juce_gui_extra.h>

namespace devildj::ui
{

class CodeTokeniser final : public juce::CodeTokeniser
{
public:
    enum Token
    {
        Error,
        Comment,
        Keyword,
        Identifier,
        ApiName,     // a Code Beat function or method: s, note, stack, fast, every ...
        Number,
        Quote,       // the quote characters of a string
        Operator,
        Bracket,
        Punctuation,
        MiniWord,    // inside a string: a sound / note name
        MiniNumber,
        MiniRest,    // ~ and -
        MiniOperator, // [ ] < > , * / ! @ ? ( ) : and the rest
        Count
    };

    int readNextToken(juce::CodeDocument::Iterator& source) override;
    juce::CodeEditorComponent::ColourScheme getDefaultColourScheme() override;

    static bool isApiName(const juce::String& word);
    static std::vector<juce::String> apiNames(); // every name it colours as Code Beat's
    static bool isKeyword(const juce::String& word);
};

} // namespace devildj::ui
