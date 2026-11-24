#include "ui/CodeHelp.h"

#include "CodeBeatHelp.h" // the embedded reference.json and examples (juce_add_binary_data)

#include <algorithm>

namespace devildj::ui
{

namespace
{
const char kScales[][16] = {"major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "locrian",
                            "harmonicMinor", "melodicMinor", "majPent", "minPent", "blues", "chromatic"};

juce::String resource(const juce::String& fileName)
{
    for (int i = 0; i < CodeBeatHelp::namedResourceListSize; ++i)
        if (fileName == CodeBeatHelp::originalFilenames[i])
        {
            int size = 0;
            const char* data = CodeBeatHelp::getNamedResource(CodeBeatHelp::namedResourceList[i], size);
            return juce::String::fromUTF8(data, size);
        }
    return {};
}

bool isIdentifierChar(juce::juce_wchar c)
{
    return juce::CharacterFunctions::isLetterOrDigit(c) || c == '_' || c == '$';
}

bool isMiniWordChar(juce::juce_wchar c)
{
    return juce::CharacterFunctions::isLetterOrDigit(c) || c == '#' || c == '_' || c == '.';
}

// Whether `index` is inside a string literal: its line scanned from the start (comments and escapes known).
bool insideString(const juce::String& text, int index)
{
    int lineStart = index;
    while (lineStart > 0 && text[lineStart - 1] != '\n')
        --lineStart;
    juce::juce_wchar quote = 0;
    for (int i = lineStart; i < index; ++i)
    {
        const auto c = text[i];
        if (quote != 0)
        {
            if (c == '\\')
                ++i;
            else if (c == quote)
                quote = 0;
        }
        else if (c == '/' && text[i + 1] == '/')
            return false;
        else if (c == '"' || c == '\'' || c == '`')
            quote = c;
    }
    return quote != 0;
}
} // namespace

const std::vector<HelpEntry>& codeReference()
{
    static const std::vector<HelpEntry> entries = []
    {
        std::vector<HelpEntry> out;
        const auto json = juce::JSON::parse(resource("reference.json"));
        if (const auto* list = json.getProperty("entries", {}).getArray())
            for (const auto& e : *list)
                out.push_back({e.getProperty("name", "").toString(), e.getProperty("kind", "").toString(),
                               e.getProperty("signature", "").toString(), e.getProperty("help", "").toString(),
                               e.getProperty("example", "").toString()});
        return out;
    }();
    return entries;
}

const HelpEntry* findHelp(const juce::String& name)
{
    for (const auto& e : codeReference())
        if (e.name == name)
            return &e;
    return nullptr;
}

const std::vector<CodeExample>& codeExamples()
{
    static const std::vector<CodeExample> examples = []
    {
        std::vector<std::pair<juce::String, juce::String>> files; // file name, code
        for (int i = 0; i < CodeBeatHelp::namedResourceListSize; ++i)
        {
            const juce::String name = CodeBeatHelp::originalFilenames[i];
            if (name.endsWith(".js"))
                files.emplace_back(name, resource(name));
        }
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first.compareNatural(b.first) < 0; });
        std::vector<CodeExample> out;
        for (const auto& [file, code] : files)
        {
            auto title = code.upToFirstOccurrenceOf("\n", false, false).trim().fromFirstOccurrenceOf("//", false, false).trim();
            title = title.upToFirstOccurrenceOf(" - ", false, false).trim();
            out.push_back({title.isEmpty() ? file.upToLastOccurrenceOf(".", false, false) : title, code});
        }
        return out;
    }();
    return examples;
}

juce::String wordAt(const juce::String& text, int index, int* start)
{
    if (index < 0 || index > text.length())
        return {};
    int a = index, b = index;
    while (a > 0 && isIdentifierChar(text[a - 1]))
        --a;
    while (b < text.length() && isIdentifierChar(text[b]))
        ++b;
    if (start != nullptr)
        *start = a;
    return text.substring(a, b);
}

Completion completionsAt(const juce::String& text, int caret, const std::vector<juce::String>& soundNames)
{
    Completion c;
    caret = std::clamp(caret, 0, text.length());
    c.inString = insideString(text, caret);
    int start = caret;
    const auto wordChar = c.inString ? isMiniWordChar : isIdentifierChar;
    while (start > 0 && wordChar(text[start - 1]))
        --start;
    c.start = start;
    c.prefix = text.substring(start, caret);
    std::vector<juce::String> pool;
    if (c.inString)
    {
        if (start > 0 && text[start - 1] == ':')
            for (const auto* s : kScales) // "C:mi|" -> the scales
                pool.emplace_back(s);
        else
            pool = soundNames;
    }
    else
    {
        const bool method = start > 0 && text[start - 1] == '.';
        for (const auto& e : codeReference())
        {
            const bool isMethodLike = e.kind == "method" || e.kind == "control";
            const bool isFunctionLike = e.kind != "method";
            if (method ? isMethodLike : isFunctionLike)
                pool.push_back(e.name);
        }
    }
    for (const auto& name : pool)
        if (name.startsWithIgnoreCase(c.prefix))
            c.options.push_back(name);
    std::sort(c.options.begin(), c.options.end(), [&](const juce::String& a, const juce::String& b)
              {
                  const bool ea = a.startsWith(c.prefix), eb = b.startsWith(c.prefix);
                  if (ea != eb)
                      return ea; // the exact-case matches first
                  return a.compareIgnoreCase(b) < 0;
              });
    c.options.erase(std::unique(c.options.begin(), c.options.end()), c.options.end());
    return c;
}

} // namespace devildj::ui
