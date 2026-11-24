#include "codebeat/PreTokenizer.h"

#include <cstdint>

namespace devildj::codebeat
{

namespace
{
// Bytes in the UTF-8 sequence that starts with `lead` (1 for anything that isn't a valid lead byte).
int sequenceLength(unsigned char lead)
{
    if (lead < 0x80)
        return 1;
    if ((lead & 0xE0) == 0xC0)
        return 2;
    if ((lead & 0xF0) == 0xE0)
        return 3;
    if ((lead & 0xF8) == 0xF0)
        return 4;
    return 1;
}

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
} // namespace

int PreTokenized::originalColumn(int line, int column) const
{
    int shift = 0;
    for (const auto& i : insertions)
        if (i.line == line && i.column < column)
            shift += i.characters;
    return column - shift;
}

PreTokenized preTokenize(std::string_view code)
{
    PreTokenized out;
    out.code.reserve(code.size() + code.size() / 4);
    int chars = 0;       // characters of the original read so far
    int line = 1;
    int outColumn = 1;   // in the rewritten line
    char lastSignificant = 0; // the last character that isn't a space or in a comment
    std::size_t i = 0;

    // Copies one character (a whole UTF-8 sequence) of the original.
    const auto copy = [&]
    {
        const int n = sequenceLength(static_cast<unsigned char>(code[i]));
        for (int k = 0; k < n && i < code.size(); ++k)
            out.code += code[i++];
        ++chars;
        if (out.code.back() == '\n')
        {
            ++line;
            outColumn = 1;
        }
        else
            ++outColumn;
    };
    const auto insert = [&](const std::string& text)
    {
        out.insertions.push_back({line, outColumn, static_cast<int>(text.size())});
        out.code += text;
        outColumn += static_cast<int>(text.size());
    };

    while (i < code.size())
    {
        const char c = code[i];
        const char next = i + 1 < code.size() ? code[i + 1] : '\0';
        if (c == '/' && next == '/')
        {
            while (i < code.size() && code[i] != '\n')
                copy();
            continue;
        }
        if (c == '/' && next == '*')
        {
            copy();
            copy();
            while (i < code.size() && !(code[i] == '*' && i + 1 < code.size() && code[i + 1] == '/'))
                copy();
            if (i < code.size())
            {
                copy();
                copy();
            }
            continue;
        }
        if (c == '"' || c == '\'' || c == '`')
        {
            // The literal's extent (escapes skipped), and whether a template has ${...}.
            std::size_t end = i + 1;
            bool interpolated = false;
            while (end < code.size() && code[end] != c)
            {
                if (code[end] == '\\')
                    ++end;
                else if (c == '`' && code[end] == '$' && end + 1 < code.size() && code[end + 1] == '{')
                    interpolated = true;
                else if (c != '`' && code[end] == '\n')
                    break; // an unclosed string: leave it for the parser to report
                ++end;
            }
            const bool closed = end < code.size() && code[end] == c;
            // Argument position: after "(", "," or "[" - and not an object key ("b": after a comma).
            std::size_t after = end + 1;
            while (after < code.size() && isSpace(code[after]))
                ++after;
            const bool key = after < code.size() && code[after] == ':';
            const bool argument = lastSignificant == '(' || lastSignificant == ',' || lastSignificant == '[';
            if (closed && !interpolated && argument && !key)
            {
                const int offset = chars + 1; // the first character inside the quotes
                insert("__mini(");
                while (i <= end)
                    copy();
                insert(", " + std::to_string(offset) + ")");
            }
            else
                while (i < code.size() && i <= end)
                    copy();
            lastSignificant = c;
            continue;
        }
        if (!isSpace(c))
            lastSignificant = c;
        copy();
    }
    return out;
}

std::pair<int, int> lineColumnOf(std::string_view code, int characterOffset)
{
    int line = 1, column = 1, chars = 0;
    for (std::size_t i = 0; i < code.size() && chars < characterOffset;)
    {
        if (code[i] == '\n')
        {
            ++line;
            column = 1;
        }
        else
            ++column;
        i += static_cast<std::size_t>(sequenceLength(static_cast<unsigned char>(code[i])));
        ++chars;
    }
    return {line, column};
}

} // namespace devildj::codebeat
