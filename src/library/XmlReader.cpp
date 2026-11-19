#include "XmlReader.h"

#include <charconv>

namespace devildj::library
{

const std::string* XmlElement::attribute(const std::string& key) const
{
    const auto found = attributes.find(key);
    return found == attributes.end() ? nullptr : &found->second;
}

std::string XmlElement::attributeOr(const std::string& key, const std::string& fallback) const
{
    const auto* v = attribute(key);
    return v != nullptr ? *v : fallback;
}

const XmlElement* XmlElement::child(std::string_view n) const
{
    for (const auto& c : children)
        if (c->name == n)
            return c.get();
    return nullptr;
}

std::vector<const XmlElement*> XmlElement::childrenNamed(std::string_view n) const
{
    std::vector<const XmlElement*> out;
    for (const auto& c : children)
        if (c->name == n)
            out.push_back(c.get());
    return out;
}

namespace
{
void appendUtf8(std::string& out, unsigned code)
{
    if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
        code = 0xFFFD;
    if (code < 0x80)
        out += static_cast<char>(code);
    else if (code < 0x800)
    {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else if (code < 0x10000)
    {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

class Parser
{
public:
    explicit Parser(std::string_view t)
        : text(t)
    {
        if (text.substr(0, 3) == "\xEF\xBB\xBF")
            pos = 3;
    }

    util::Result<std::unique_ptr<XmlElement>> document()
    {
        using R = util::Result<std::unique_ptr<XmlElement>>;
        skipMisc();
        if (!startsWith("<"))
            return R::fail("not an XML file");
        auto root = element();
        if (!root)
            return R::fail(error.empty() ? "broken XML" : error);
        skipMisc();
        if (pos != text.size())
            return R::fail("text after the end of the document");
        return R::ok(std::move(root));
    }

private:
    bool startsWith(std::string_view s) const { return text.substr(pos, s.size()) == s; }
    bool fail(std::string why)
    {
        if (error.empty())
            error = why + " (at byte " + std::to_string(pos) + ")";
        return false;
    }
    void skipSpace()
    {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n'))
            ++pos;
    }
    bool skipPast(std::string_view end)
    {
        const auto found = text.find(end, pos);
        if (found == std::string_view::npos)
            return fail("unterminated markup");
        pos = found + end.size();
        return true;
    }
    // Declarations, comments, processing instructions and a DOCTYPE before / after the root.
    void skipMisc()
    {
        for (;;)
        {
            skipSpace();
            if (startsWith("<?"))
                skipPast("?>");
            else if (startsWith("<!--"))
                skipPast("-->");
            else if (startsWith("<!DOCTYPE"))
            {
                // Skip it, including an internal subset in [...]; entities in it are not used.
                int brackets = 0;
                while (pos < text.size())
                {
                    const char c = text[pos++];
                    if (c == '[')
                        ++brackets;
                    else if (c == ']')
                        --brackets;
                    else if (c == '>' && brackets <= 0)
                        break;
                }
            }
            else
                return;
        }
    }

    bool decode(std::string_view raw, std::string& out)
    {
        for (std::size_t i = 0; i < raw.size(); ++i)
        {
            if (raw[i] != '&')
            {
                out += raw[i];
                continue;
            }
            const auto semi = raw.find(';', i);
            if (semi == std::string_view::npos)
                return fail("an & without ;");
            const auto name = raw.substr(i + 1, semi - i - 1);
            if (name == "amp")
                out += '&';
            else if (name == "lt")
                out += '<';
            else if (name == "gt")
                out += '>';
            else if (name == "quot")
                out += '"';
            else if (name == "apos")
                out += '\'';
            else if (name.size() > 1 && name[0] == '#')
            {
                unsigned code = 0;
                const bool hex = name[1] == 'x' || name[1] == 'X';
                const auto digits = name.substr(hex ? 2 : 1);
                const auto [p, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), code, hex ? 16 : 10);
                if (ec != std::errc{} || p != digits.data() + digits.size())
                    return fail("a bad character reference");
                appendUtf8(out, code);
            }
            else
                return fail("an unknown entity &" + std::string(name) + ";");
            i = semi;
        }
        return true;
    }

    bool name(std::string& out)
    {
        const auto start = pos;
        while (pos < text.size())
        {
            const char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/' || c == '>' || c == '=' || c == '<')
                break;
            ++pos;
        }
        if (pos == start)
            return fail("a missing name");
        out = std::string(text.substr(start, pos - start));
        return true;
    }

    std::unique_ptr<XmlElement> element()
    {
        if (++depth > 256)
        {
            fail("nested too deeply");
            return nullptr;
        }
        ++pos; // '<'
        auto e = std::make_unique<XmlElement>();
        if (!name(e->name))
            return nullptr;
        for (;;)
        {
            skipSpace();
            if (startsWith("/>"))
            {
                pos += 2;
                --depth;
                return e;
            }
            if (startsWith(">"))
            {
                ++pos;
                break;
            }
            std::string key;
            if (!name(key))
                return nullptr;
            skipSpace();
            if (!startsWith("="))
            {
                fail("an attribute without a value");
                return nullptr;
            }
            ++pos;
            skipSpace();
            if (pos >= text.size() || (text[pos] != '"' && text[pos] != '\''))
            {
                fail("an attribute value without quotes");
                return nullptr;
            }
            const char quote = text[pos++];
            const auto end = text.find(quote, pos);
            if (end == std::string_view::npos)
            {
                fail("an unterminated attribute value");
                return nullptr;
            }
            std::string value;
            if (!decode(text.substr(pos, end - pos), value))
                return nullptr;
            e->attributes[key] = std::move(value);
            pos = end + 1;
        }

        // Content up to the matching end tag.
        for (;;)
        {
            if (pos >= text.size())
            {
                fail("<" + e->name + "> is never closed");
                return nullptr;
            }
            if (startsWith("</"))
            {
                pos += 2;
                std::string closing;
                if (!name(closing))
                    return nullptr;
                skipSpace();
                if (closing != e->name || !startsWith(">"))
                {
                    fail("</" + closing + "> doesn't close <" + e->name + ">");
                    return nullptr;
                }
                ++pos;
                --depth;
                return e;
            }
            if (startsWith("<!--"))
            {
                if (!skipPast("-->"))
                    return nullptr;
                continue;
            }
            if (startsWith("<![CDATA["))
            {
                pos += 9;
                const auto end = text.find("]]>", pos);
                if (end == std::string_view::npos)
                {
                    fail("an unterminated CDATA section");
                    return nullptr;
                }
                e->text += text.substr(pos, end - pos);
                pos = end + 3;
                continue;
            }
            if (startsWith("<?"))
            {
                if (!skipPast("?>"))
                    return nullptr;
                continue;
            }
            if (startsWith("<"))
            {
                auto c = element();
                if (!c)
                    return nullptr;
                e->children.push_back(std::move(c));
                continue;
            }
            const auto next = text.find('<', pos);
            const auto end = next == std::string_view::npos ? text.size() : next;
            if (!decode(text.substr(pos, end - pos), e->text))
                return nullptr;
            pos = end;
        }
    }

    std::string_view text;
    std::size_t pos = 0;
    int depth = 0;
    std::string error;
};
} // namespace

util::Result<std::unique_ptr<XmlElement>> parseXml(std::string_view text)
{
    return Parser(text).document();
}

} // namespace devildj::library
