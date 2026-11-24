#include "ui/CodeTokeniser.h"

#include <set>

namespace devildj::ui
{

namespace
{
const std::set<juce::String> kKeywords{
    "await", "break", "case", "catch", "class", "const", "continue", "default", "delete", "do", "else", "export",
    "extends", "false", "finally", "for", "function", "if", "import", "in", "instanceof", "let", "new", "null",
    "of", "return", "static", "super", "switch", "this", "throw", "true", "try", "typeof", "undefined", "var",
    "void", "while", "yield"};

// The language's names (CODEBEAT_SPEC §5): constructors, program functions, controls, signals and methods.
const std::set<juce::String> kApi{
    // constructors and program functions
    "pure", "silence", "seq", "fastcat", "cat", "slowcat", "stack", "timecat", "mini", "run", "euclid", "signal",
    "play", "mute", "unmute", "hush", "setbpm", "setcpb", "print", "deckA", "deckB", "deckC", "deckD", "master",
    // signals
    "sine", "cosine", "saw", "isaw", "tri", "square", "rand", "irand", "perlin",
    // controls
    "s", "sound", "n", "note", "gain", "pan", "speed", "lpf", "cutoff", "hpf", "resonance", "attack", "decay",
    "sustain", "release", "legato", "room", "delay", "crush", "cut", "bank",
    // methods
    "fast", "slow", "early", "late", "rev", "every", "whenCycle", "iter", "palindrome", "ply", "superimpose", "layer",
    "off", "degradeBy", "degrade", "sometimesBy", "sometimes", "often", "rarely", "seed", "struct", "mask", "add",
    "sub", "mul", "div", "scale", "hurry", "range", "segment", "fmap"};

bool isIdentifierStart(juce::juce_wchar c)
{
    return juce::CharacterFunctions::isLetter(c) || c == '_' || c == '$';
}

bool isIdentifierChar(juce::juce_wchar c)
{
    return juce::CharacterFunctions::isLetterOrDigit(c) || c == '_' || c == '$';
}

bool isDigit(juce::juce_wchar c)
{
    return c >= '0' && c <= '9';
}

// Inside which string (its quote character) the iterator is, 0 if none: its line scanned from the start.
juce::juce_wchar openQuote(const juce::CodeDocument::Iterator& at)
{
    auto it = at;
    it.skipToStartOfLine();
    juce::juce_wchar quote = 0;
    bool blockComment = false;
    while (it.getPosition() < at.getPosition() && !it.isEOF())
    {
        const auto c = it.nextChar();
        if (blockComment)
        {
            if (c == '*' && it.peekNextChar() == '/')
            {
                it.skip();
                blockComment = false;
            }
            continue;
        }
        if (quote != 0)
        {
            if (c == '\\')
                it.skip(); // an escaped character
            else if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '/' && it.peekNextChar() == '/')
            return 0; // the rest of the line is a comment (the caller is past it only if it is in it)
        if (c == '/' && it.peekNextChar() == '*')
        {
            it.skip();
            blockComment = true;
        }
        else if (c == '"' || c == '\'' || c == '`')
            quote = c;
    }
    return quote;
}

int readMini(juce::CodeDocument::Iterator& source, juce::juce_wchar quote)
{
    const auto c = source.peekNextChar();
    if (c == quote)
    {
        source.skip();
        return CodeTokeniser::Quote;
    }
    if (c == '\\')
    {
        source.skip();
        source.skip();
        return CodeTokeniser::MiniOperator;
    }
    if (juce::CharacterFunctions::isLetter(c))
    {
        while (!source.isEOF())
        {
            const auto d = source.peekNextChar();
            if (!(juce::CharacterFunctions::isLetterOrDigit(d) || d == '#' || d == '_' || d == '.'))
                break;
            source.skip();
        }
        return CodeTokeniser::MiniWord;
    }
    auto next = source;
    next.skip();
    const auto after = next.peekNextChar();
    if (isDigit(c) || ((c == '.' || c == '-') && isDigit(after)))
    {
        source.skip();
        while (!source.isEOF() && (isDigit(source.peekNextChar()) || source.peekNextChar() == '.'))
            source.skip();
        return CodeTokeniser::MiniNumber;
    }
    source.skip();
    if (c == '~' || c == '-')
        return CodeTokeniser::MiniRest;
    if (c == '\n' || c == '\r')
        return CodeTokeniser::Punctuation;
    return CodeTokeniser::MiniOperator;
}
} // namespace

bool CodeTokeniser::isApiName(const juce::String& word)
{
    return kApi.count(word) > 0;
}

bool CodeTokeniser::isKeyword(const juce::String& word)
{
    return kKeywords.count(word) > 0;
}

int CodeTokeniser::readNextToken(juce::CodeDocument::Iterator& source)
{
    // Spaces belong to the token after them; inside a string they do too (they separate mini-notation steps).
    if (const auto quote = openQuote(source); quote != 0)
    {
        if (juce::CharacterFunctions::isWhitespace(source.peekNextChar()) && source.peekNextChar() != '\n')
        {
            while (!source.isEOF() && source.peekNextChar() == ' ')
                source.skip();
            if (source.isEOF())
                return Punctuation;
        }
        return readMini(source, quote);
    }
    source.skipWhitespace();
    if (source.isEOF())
        return Punctuation;
    const auto c = source.peekNextChar();
    auto next = source;
    next.skip();
    const auto after = next.peekNextChar();

    if (c == '/' && after == '/')
    {
        source.skipToEndOfLine();
        return Comment;
    }
    if (c == '/' && after == '*')
    {
        source.skip();
        source.skip();
        while (!source.isEOF())
        {
            const auto d = source.nextChar();
            if (d == '*' && source.peekNextChar() == '/')
            {
                source.skip();
                break;
            }
            if (d == '\n')
                break;
        }
        return Comment;
    }
    if (c == '"' || c == '\'' || c == '`')
    {
        source.skip();
        return Quote;
    }
    if (isDigit(c) || (c == '.' && isDigit(after)))
    {
        source.skip();
        while (!source.isEOF())
        {
            const auto d = source.peekNextChar();
            if (!(juce::CharacterFunctions::isLetterOrDigit(d) || d == '.' || d == '_'))
                break;
            source.skip();
        }
        return Number;
    }
    if (isIdentifierStart(c))
    {
        juce::String word;
        while (!source.isEOF() && isIdentifierChar(source.peekNextChar()))
            word += juce::String::charToString(source.nextChar());
        if (isKeyword(word))
            return Keyword;
        return isApiName(word) ? ApiName : Identifier;
    }
    source.skip();
    if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}')
        return Bracket;
    if (juce::String("+-*/%=<>!&|^?:~").containsChar(c))
        return Operator;
    return Punctuation;
}

juce::CodeEditorComponent::ColourScheme CodeTokeniser::getDefaultColourScheme()
{
    juce::CodeEditorComponent::ColourScheme cs;
    const struct
    {
        const char* name;
        juce::uint32 colour;
    } types[] = {
        {"Error", 0xffe05050},        {"Comment", 0xff6a737d},     {"Keyword", 0xffc792ea},
        {"Identifier", 0xffd8d8e0},   {"Code Beat", 0xff5fd38a},   {"Number", 0xfff2a65a},
        {"Quote", 0xff8a8a96},        {"Operator", 0xff89ddff},    {"Bracket", 0xffb0b0bc},
        {"Punctuation", 0xffb0b0bc},  {"Sound", 0xffffcb6b},       {"Mini number", 0xfff2a65a},
        {"Rest", 0xff6a737d},         {"Mini operator", 0xff89ddff},
    };
    static_assert(std::size(types) == Count);
    for (const auto& t : types)
        cs.set(t.name, juce::Colour(t.colour));
    return cs;
}

} // namespace devildj::ui
