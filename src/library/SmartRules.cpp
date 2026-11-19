#include "SmartRules.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <memory>
#include <optional>

namespace devildj::library
{

namespace
{
constexpr std::int64_t kDay = 86400;

struct FieldInfo
{
    RuleField field;
    const char* name;
    const char* column; // on `tracks t`
};

const FieldInfo kFields[] = {
    {RuleField::Title, "title", "t.title"},
    {RuleField::Artist, "artist", "t.artist"},
    {RuleField::Album, "album", "t.album"},
    {RuleField::Genre, "genre", "t.genre"},
    {RuleField::Comment, "comment", "t.comment"},
    {RuleField::Bpm, "bpm", "t.bpm"},
    {RuleField::Key, "key", "t.musical_key"},
    {RuleField::Rating, "rating", "t.rating"},
    {RuleField::Colour, "colour", "t.color"},
    {RuleField::Year, "year", "t.year"},
    {RuleField::Time, "time", "t.duration_ms"},
    {RuleField::DateAdded, "date_added", "t.date_added"},
    {RuleField::LastPlayed, "last_played", "t.last_played"},
    {RuleField::PlayCount, "play_count", "t.play_count"},
    {RuleField::Tag, "tag", ""},
};

const std::pair<RuleOp, const char*> kOps[] = {
    {RuleOp::Contains, "contains"},     {RuleOp::NotContains, "not_contains"}, {RuleOp::Is, "is"},
    {RuleOp::IsNot, "is_not"},           {RuleOp::StartsWith, "starts_with"},   {RuleOp::GreaterThan, "greater"},
    {RuleOp::LessThan, "less"},          {RuleOp::Between, "between"},          {RuleOp::InLast, "in_last"},
    {RuleOp::NotInLast, "not_in_last"}, {RuleOp::IsEmpty, "is_empty"},         {RuleOp::IsNotEmpty, "is_not_empty"},
};

const FieldInfo& info(RuleField f)
{
    for (const auto& i : kFields)
        if (i.field == f)
            return i;
    return kFields[0];
}

std::string likePattern(const std::string& text, bool leading, bool trailing)
{
    std::string p = leading ? "%" : "";
    for (const char c : text)
    {
        if (c == '%' || c == '_' || c == '\\')
            p += '\\';
        p += c;
    }
    return trailing ? p + "%" : p;
}

bool isWholeNumberField(RuleField f)
{
    return f == RuleField::Key || f == RuleField::Rating || f == RuleField::Colour || f == RuleField::Year ||
           f == RuleField::PlayCount;
}
} // namespace

bool isTextField(RuleField f) noexcept
{
    return f == RuleField::Title || f == RuleField::Artist || f == RuleField::Album || f == RuleField::Genre ||
           f == RuleField::Comment || f == RuleField::Tag;
}

const char* fieldName(RuleField f) noexcept
{
    return info(f).name;
}

const char* opName(RuleOp op) noexcept
{
    for (const auto& [o, name] : kOps)
        if (o == op)
            return name;
    return "?";
}

std::vector<RuleOp> operatorsFor(RuleField f)
{
    using O = RuleOp;
    switch (f)
    {
        case RuleField::Title:
        case RuleField::Artist:
        case RuleField::Album:
        case RuleField::Genre:
        case RuleField::Comment:
            return {O::Contains, O::NotContains, O::Is, O::IsNot, O::StartsWith, O::IsEmpty, O::IsNotEmpty};
        case RuleField::Tag: return {O::Is, O::IsNot};
        case RuleField::Key:
        case RuleField::Colour: return {O::Is, O::IsNot, O::IsEmpty, O::IsNotEmpty};
        case RuleField::DateAdded: return {O::InLast, O::NotInLast};
        case RuleField::LastPlayed: return {O::InLast, O::NotInLast, O::IsEmpty, O::IsNotEmpty};
        case RuleField::Bpm:
        case RuleField::Rating:
        case RuleField::Year:
        case RuleField::Time:
        case RuleField::PlayCount:
            return {O::Is, O::IsNot, O::GreaterThan, O::LessThan, O::Between, O::IsEmpty, O::IsNotEmpty};
    }
    return {};
}

util::Result<SqlCondition> toSql(const SmartRules& rules, std::int64_t now)
{
    using R = util::Result<SqlCondition>;
    SqlCondition out;
    std::vector<std::string> parts;
    for (const auto& r : rules.rules)
    {
        const auto ops = operatorsFor(r.field);
        if (std::find(ops.begin(), ops.end(), r.op) == ops.end())
            return R::fail(std::string("\"") + opName(r.op) + "\" doesn't work with " + fieldName(r.field));
        const std::string col = info(r.field).column;
        // Numbers as the column stores them: seconds -> ms for Time, whole numbers for keys / ratings / colours.
        const auto number = [&](double v) -> SqlParam
        {
            if (r.field == RuleField::Time)
                return static_cast<std::int64_t>(std::llround(v * 1000.0));
            if (isWholeNumberField(r.field))
                return static_cast<std::int64_t>(std::llround(v));
            return v;
        };
        std::string sql;
        if (r.field == RuleField::Tag)
        {
            sql = std::string(r.op == RuleOp::IsNot ? "NOT " : "") +
                  "EXISTS (SELECT 1 FROM track_tags tt JOIN tags g ON g.id = tt.tag_id WHERE tt.track_id = t.id "
                  "AND g.name = ? COLLATE NOCASE)";
            out.params.push_back(r.text);
        }
        else if (isTextField(r.field))
        {
            switch (r.op)
            {
                case RuleOp::Contains:
                    sql = col + " LIKE ? ESCAPE '\\'";
                    out.params.push_back(likePattern(r.text, true, true));
                    break;
                case RuleOp::NotContains:
                    sql = "coalesce(" + col + ", '') NOT LIKE ? ESCAPE '\\'";
                    out.params.push_back(likePattern(r.text, true, true));
                    break;
                case RuleOp::StartsWith:
                    sql = col + " LIKE ? ESCAPE '\\'";
                    out.params.push_back(likePattern(r.text, false, true));
                    break;
                case RuleOp::Is:
                    sql = col + " = ? COLLATE NOCASE";
                    out.params.push_back(r.text);
                    break;
                case RuleOp::IsNot:
                    sql = "coalesce(" + col + ", '') <> ? COLLATE NOCASE";
                    out.params.push_back(r.text);
                    break;
                case RuleOp::IsEmpty: sql = "coalesce(" + col + ", '') = ''"; break;
                case RuleOp::IsNotEmpty: sql = "coalesce(" + col + ", '') <> ''"; break;
                default: break;
            }
        }
        else
        {
            switch (r.op)
            {
                case RuleOp::Is:
                    sql = col + " = ?";
                    out.params.push_back(number(r.value));
                    break;
                case RuleOp::IsNot:
                    sql = "(" + col + " IS NULL OR " + col + " <> ?)";
                    out.params.push_back(number(r.value));
                    break;
                case RuleOp::GreaterThan:
                    sql = col + " > ?";
                    out.params.push_back(number(r.value));
                    break;
                case RuleOp::LessThan:
                    sql = col + " < ?";
                    out.params.push_back(number(r.value));
                    break;
                case RuleOp::Between:
                    sql = col + " BETWEEN ? AND ?";
                    out.params.push_back(number(std::min(r.value, r.to)));
                    out.params.push_back(number(std::max(r.value, r.to)));
                    break;
                case RuleOp::InLast:
                    sql = col + " >= ?";
                    out.params.push_back(now - static_cast<std::int64_t>(std::llround(r.value)) * kDay);
                    break;
                case RuleOp::NotInLast:
                    sql = "coalesce(" + col + ", 0) < ?";
                    out.params.push_back(now - static_cast<std::int64_t>(std::llround(r.value)) * kDay);
                    break;
                case RuleOp::IsEmpty:
                    // Rating 0 and play count 0 mean "none"; the others are NULL when unknown.
                    sql = r.field == RuleField::Rating || r.field == RuleField::PlayCount
                              ? "coalesce(" + col + ", 0) = 0"
                              : col + " IS NULL";
                    break;
                case RuleOp::IsNotEmpty:
                    sql = r.field == RuleField::Rating || r.field == RuleField::PlayCount
                              ? "coalesce(" + col + ", 0) <> 0"
                              : col + " IS NOT NULL";
                    break;
                default: break;
            }
        }
        parts.push_back("(" + sql + ")");
    }
    if (parts.empty())
    {
        out.sql = "1";
        return R::ok(std::move(out));
    }
    std::string joined;
    for (const auto& p : parts)
        joined += (joined.empty() ? "" : rules.matchAll ? " AND " : " OR ") + p;
    out.sql = "(" + joined + ")";
    return R::ok(std::move(out));
}

// --- JSON ------------------------------------------------------------------------------------------------------

namespace
{
std::string quote(const std::string& s)
{
    std::string out = "\"";
    for (const char c : s)
    {
        switch (c)
        {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out += c;
                }
        }
    }
    return out + "\"";
}

std::string number(double v)
{
    char buf[32];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    return ec == std::errc{} ? std::string(buf, end) : "0";
}

// A small JSON reader for the rule format: objects, arrays, strings (with \u escapes), numbers, true / false /
// null. Enough for rule_json; not a general-purpose parser.
struct Json
{
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    } type = Type::Null;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<Json> items;
    std::map<std::string, Json> fields;
};

class Reader
{
public:
    explicit Reader(std::string_view t)
        : text(t)
    {
    }

    std::optional<Json> document()
    {
        auto v = value();
        skip();
        if (!v || pos != text.size())
            return std::nullopt;
        return v;
    }

private:
    void skip()
    {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n' || text[pos] == '\r' || text[pos] == '\t'))
            ++pos;
    }
    bool eat(char c)
    {
        skip();
        if (pos < text.size() && text[pos] == c)
        {
            ++pos;
            return true;
        }
        return false;
    }
    std::optional<std::string> string()
    {
        if (!eat('"'))
            return std::nullopt;
        std::string out;
        while (pos < text.size())
        {
            const char c = text[pos++];
            if (c == '"')
                return out;
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (pos >= text.size())
                return std::nullopt;
            const char e = text[pos++];
            switch (e)
            {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u':
                {
                    if (pos + 4 > text.size())
                        return std::nullopt;
                    unsigned code = 0;
                    const auto [p, ec] = std::from_chars(text.data() + pos, text.data() + pos + 4, code, 16);
                    if (ec != std::errc{} || p != text.data() + pos + 4)
                        return std::nullopt;
                    pos += 4;
                    // UTF-8 (surrogate pairs aren't needed for this format; they become U+FFFD).
                    if (code >= 0xD800 && code <= 0xDFFF)
                        code = 0xFFFD;
                    if (code < 0x80)
                        out += static_cast<char>(code);
                    else if (code < 0x800)
                    {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    else
                    {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default: return std::nullopt;
            }
        }
        return std::nullopt;
    }
    std::optional<Json> value()
    {
        skip();
        if (pos >= text.size())
            return std::nullopt;
        Json j;
        const char c = text[pos];
        if (c == '{')
        {
            ++pos;
            j.type = Json::Type::Object;
            if (eat('}'))
                return j;
            do
            {
                auto key = string();
                if (!key || !eat(':'))
                    return std::nullopt;
                auto v = value();
                if (!v)
                    return std::nullopt;
                j.fields[*key] = std::move(*v);
            } while (eat(','));
            return eat('}') ? std::optional<Json>(std::move(j)) : std::nullopt;
        }
        if (c == '[')
        {
            ++pos;
            j.type = Json::Type::Array;
            if (eat(']'))
                return j;
            do
            {
                auto v = value();
                if (!v)
                    return std::nullopt;
                j.items.push_back(std::move(*v));
            } while (eat(','));
            return eat(']') ? std::optional<Json>(std::move(j)) : std::nullopt;
        }
        if (c == '"')
        {
            auto s = string();
            if (!s)
                return std::nullopt;
            j.type = Json::Type::String;
            j.s = std::move(*s);
            return j;
        }
        for (const auto& [word, type, flag] : {std::tuple{"true", Json::Type::Bool, true},
                                               std::tuple{"false", Json::Type::Bool, false},
                                               std::tuple{"null", Json::Type::Null, false}})
            if (text.substr(pos, std::string_view(word).size()) == word)
            {
                pos += std::string_view(word).size();
                j.type = type;
                j.b = flag;
                return j;
            }
        const auto [p, ec] = std::from_chars(text.data() + pos, text.data() + text.size(), j.n);
        if (ec != std::errc{} || p == text.data() + pos)
            return std::nullopt;
        pos = static_cast<std::size_t>(p - text.data());
        j.type = Json::Type::Number;
        return j;
    }

    std::string_view text;
    std::size_t pos = 0;
};
} // namespace

std::string toJson(const SmartRules& rules)
{
    std::string out = std::string("{\"match\":\"") + (rules.matchAll ? "all" : "any") + "\",\"rules\":[";
    for (std::size_t i = 0; i < rules.rules.size(); ++i)
    {
        const auto& r = rules.rules[i];
        out += (i == 0 ? "" : ",") + std::string("{\"field\":\"") + fieldName(r.field) + "\",\"op\":\"" +
               opName(r.op) + "\"";
        if (isTextField(r.field))
            out += ",\"text\":" + quote(r.text);
        else
            out += ",\"value\":" + number(r.value) + (r.op == RuleOp::Between ? ",\"to\":" + number(r.to) : "");
        out += "}";
    }
    return out + "]}";
}

util::Result<SmartRules> parseRules(std::string_view json)
{
    using R = util::Result<SmartRules>;
    auto doc = Reader(json).document();
    if (!doc || doc->type != Json::Type::Object)
        return R::fail("Smart playlist rules aren't valid JSON");
    SmartRules out;
    if (const auto m = doc->fields.find("match"); m != doc->fields.end())
    {
        if (m->second.s != "all" && m->second.s != "any")
            return R::fail("\"match\" must be \"all\" or \"any\"");
        out.matchAll = m->second.s == "all";
    }
    const auto list = doc->fields.find("rules");
    if (list == doc->fields.end())
        return R::ok(std::move(out));
    if (list->second.type != Json::Type::Array)
        return R::fail("\"rules\" must be a list");
    for (const auto& item : list->second.items)
    {
        const auto get = [&](const char* key) -> const Json*
        {
            const auto f = item.fields.find(key);
            return f == item.fields.end() ? nullptr : &f->second;
        };
        Rule r;
        const auto* field = get("field");
        const auto* op = get("op");
        if (field == nullptr || op == nullptr)
            return R::fail("A rule needs a field and an operator");
        bool knownField = false, knownOp = false;
        for (const auto& f : kFields)
            if (field->s == f.name)
            {
                r.field = f.field;
                knownField = true;
            }
        for (const auto& [o, name] : kOps)
            if (op->s == name)
            {
                r.op = o;
                knownOp = true;
            }
        if (!knownField)
            return R::fail("Unknown rule field \"" + field->s + "\"");
        if (!knownOp)
            return R::fail("Unknown rule operator \"" + op->s + "\"");
        if (const auto* t = get("text"))
            r.text = t->s;
        if (const auto* v = get("value"))
            r.value = v->n;
        if (const auto* v = get("to"))
            r.to = v->n;
        out.rules.push_back(std::move(r));
    }
    return R::ok(std::move(out));
}

} // namespace devildj::library
