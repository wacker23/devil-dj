#include "matcher/ChoiceLog.h"

#include "library/Connection.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace devildj::matcher
{

namespace
{
std::string number(double v)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6g", v);
    return buffer;
}

std::string optionalNumber(const std::optional<double>& v, const char* none)
{
    return v && std::isfinite(*v) ? number(*v) : std::string(none);
}

const char* intentName(Intent i) noexcept
{
    switch (i)
    {
        case Intent::Down:
            return "down";
        case Intent::Up:
            return "up";
        case Intent::Switch:
            return "switch";
        case Intent::Hold:
            break;
    }
    return "hold";
}

// CSV text: quoted when it has a comma, quote or line break.
std::string csvText(const std::string& s)
{
    if (s.find_first_of(",\"\n\r") == std::string::npos)
        return s;
    std::string out = "\"";
    for (const char c : s)
        out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

std::string jsonText(const std::string& s)
{
    std::string out = "\"";
    for (const char c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        if (static_cast<unsigned char>(c) < 0x20)
        {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
            out += buffer;
            continue;
        }
        out += c;
    }
    return out + "\"";
}

// A small reader for the fixed shape shownToJson writes.
struct Reader
{
    const std::string& s;
    std::size_t at = 0;
    void space()
    {
        while (at < s.size() && (s[at] == ' ' || s[at] == '\n' || s[at] == '\t' || s[at] == '\r'))
            ++at;
    }
    bool take(char c)
    {
        space();
        if (at < s.size() && s[at] == c)
        {
            ++at;
            return true;
        }
        return false;
    }
    std::string key()
    {
        space();
        std::string k;
        if (!take('"'))
            return k;
        while (at < s.size() && s[at] != '"')
            k += s[at++];
        ++at;
        take(':');
        return k;
    }
    std::optional<double> value()
    {
        space();
        if (s.compare(at, 4, "null") == 0)
        {
            at += 4;
            return std::nullopt;
        }
        if (s.compare(at, 4, "true") == 0)
        {
            at += 4;
            return 1.0;
        }
        if (s.compare(at, 5, "false") == 0)
        {
            at += 5;
            return 0.0;
        }
        char* end = nullptr;
        const double v = std::strtod(s.c_str() + at, &end); // no exceptions on a damaged row
        const auto used = static_cast<std::size_t>(end - (s.c_str() + at));
        if (used == 0)
        {
            ++at; // skip what isn't a number
            return std::nullopt;
        }
        at += used;
        return v;
    }
};
} // namespace

const char* choiceDimName(int i) noexcept
{
    if (i >= 0 && i < kFeatureCount)
        return featureName(static_cast<FeatureId>(i));
    static const char* const extra[] = {"energy_up", "energy_down", "sound_switch"};
    const int k = i - kFeatureCount;
    return k >= 0 && k < 3 ? extra[k] : "";
}

ChoiceVector choiceVector(const std::array<std::optional<double>, kFeatureCount>& f, std::optional<double> delta,
                          Intent intent)
{
    ChoiceVector x{};
    for (int i = 0; i < kFeatureCount; ++i)
        x[static_cast<std::size_t>(i)] = f[static_cast<std::size_t>(i)];
    if (delta)
    {
        x[kFeatureCount] = intent == Intent::Up ? *delta : 0.0;
        x[kFeatureCount + 1] = intent == Intent::Down ? *delta : 0.0;
    }
    const auto& sound = f[static_cast<std::size_t>(FeatureId::Sound)];
    if (sound)
        x[kFeatureCount + 2] = intent == Intent::Switch ? *sound : 0.0;
    return x;
}

const char* outcomeName(Outcome o) noexcept
{
    switch (o)
    {
        case Outcome::Mixed:
            return "mixed";
        case Outcome::Ejected:
            return "ejected";
        case Outcome::ThumbsUp:
            return "thumbs_up";
        case Outcome::ThumbsDown:
            return "thumbs_down";
        case Outcome::Unknown:
            break;
    }
    return "unknown";
}

std::string shownToJson(const std::vector<ShownItem>& shown)
{
    std::string out = "[";
    for (std::size_t i = 0; i < shown.size(); ++i)
    {
        const auto& s = shown[i];
        out += (i > 0 ? ", " : "");
        out += "{\"track\": " + std::to_string(s.track) + ", \"rank\": " + (s.rank ? std::to_string(*s.rank) : "null") +
               ", \"chosen\": " + (s.chosen ? "true" : "false") + ", \"x\": [";
        for (int k = 0; k < kChoiceDims; ++k)
            out += (k > 0 ? ", " : "") + optionalNumber(s.x[static_cast<std::size_t>(k)], "null");
        out += "]}";
    }
    return out + "]";
}

std::vector<ShownItem> shownFromJson(const std::string& json)
{
    std::vector<ShownItem> out;
    Reader r{json};
    if (!r.take('['))
        return out;
    while (r.take('{'))
    {
        ShownItem item;
        for (;;)
        {
            const auto k = r.key();
            if (k == "x")
            {
                r.take('[');
                for (int i = 0; i < kChoiceDims; ++i)
                {
                    item.x[static_cast<std::size_t>(i)] = r.value();
                    r.take(',');
                }
                r.take(']');
            }
            else if (k == "track")
                item.track = static_cast<std::int64_t>(r.value().value_or(0.0));
            else if (k == "rank")
            {
                if (const auto v = r.value())
                    item.rank = static_cast<int>(*v);
            }
            else if (k == "chosen")
                item.chosen = r.value().value_or(0.0) != 0.0;
            else
                break;
            if (!r.take(','))
                break;
        }
        r.take('}');
        out.push_back(item);
        if (!r.take(','))
            break;
    }
    return out;
}

ChoiceLogRepository::ChoiceLogRepository(library::Connection& connection) noexcept
    : db(connection)
{
}

std::optional<std::int64_t> ChoiceLogRepository::add(const Impression& m)
{
    auto* s = db.prepare("INSERT INTO choice_log (id, session_id, created_at, context_track, chosen_track, intent, preset, "
                         "scope, shown_json, chosen_rank, outcome, model_version) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (s == nullptr)
        return std::nullopt;
    m.id > 0 ? s->bind(1, m.id) : s->bindNull(1); // the app hands ids out (its writes are asynchronous)
    s->bind(2, m.sessionId).bind(3, m.createdAt).bind(4, m.contextTrack).bind(5, m.chosenTrack);
    s->bind(6, static_cast<int>(m.intent)).bind(7, m.preset).bind(8, m.scope).bind(9, shownToJson(m.shown));
    m.chosenRank ? s->bind(10, *m.chosenRank) : s->bindNull(10);
    s->bind(11, static_cast<int>(m.outcome)).bind(12, m.modelVersion);
    if (!s->run())
        return std::nullopt;
    if (m.id > 0)
        return m.id;
    return db.queryInt("SELECT last_insert_rowid()");
}

bool ChoiceLogRepository::setOutcome(std::int64_t id, Outcome outcome)
{
    auto* s = db.prepare("UPDATE choice_log SET outcome = ? WHERE id = ?");
    return s != nullptr && s->bind(1, static_cast<int>(outcome)).bind(2, id).run();
}

std::vector<Impression> ChoiceLogRepository::query(const std::string& where, std::int64_t value)
{
    std::vector<Impression> out;
    auto* s = db.prepare("SELECT id, session_id, created_at, context_track, chosen_track, intent, preset, scope, "
                         "shown_json, chosen_rank, outcome, model_version FROM choice_log " + where + " ORDER BY id");
    if (s == nullptr)
        return out;
    if (!where.empty())
        s->bind(1, value);
    while (s->step())
    {
        Impression m;
        m.id = s->getInt(0);
        m.sessionId = s->getInt(1);
        m.createdAt = s->getInt(2);
        m.contextTrack = s->getInt(3);
        m.chosenTrack = s->getInt(4);
        m.intent = static_cast<Intent>(s->getInt(5));
        m.preset = s->getText(6);
        m.scope = s->getText(7);
        m.shown = shownFromJson(s->getText(8));
        if (const auto r = s->getOptionalInt(9))
            m.chosenRank = static_cast<int>(*r);
        m.outcome = static_cast<Outcome>(s->getInt(10));
        m.modelVersion = s->getText(11);
        out.push_back(std::move(m));
    }
    s->reset();
    return out;
}

std::vector<Impression> ChoiceLogRepository::all()
{
    return query({}, 0);
}

std::vector<Impression> ChoiceLogRepository::session(std::int64_t sessionId)
{
    return query("WHERE session_id = ?", sessionId);
}

int ChoiceLogRepository::count()
{
    return static_cast<int>(db.queryInt("SELECT count(*) FROM choice_log").value_or(0));
}

bool ChoiceLogRepository::clear()
{
    return db.exec("DELETE FROM choice_log");
}

std::optional<std::string> exportCsv(const std::vector<Impression>& impressions, const std::filesystem::path& file)
{
    std::ofstream out(file, std::ios::binary);
    if (!out)
        return "can't write " + file.string();
    out << "impression,session,created_at,context_track,chosen_track,intent,preset,scope,outcome,chosen_rank,"
           "model_version,track,rank,chosen";
    for (int k = 0; k < kChoiceDims; ++k)
        out << ',' << choiceDimName(k);
    out << '\n';
    for (const auto& m : impressions)
        for (const auto& s : m.shown)
        {
            out << m.id << ',' << m.sessionId << ',' << m.createdAt << ',' << m.contextTrack << ',' << m.chosenTrack << ','
                << intentName(m.intent) << ',' << csvText(m.preset) << ',' << csvText(m.scope) << ','
                << outcomeName(m.outcome) << ',' << (m.chosenRank ? std::to_string(*m.chosenRank) : "") << ','
                << csvText(m.modelVersion) << ',' << s.track << ',' << (s.rank ? std::to_string(*s.rank) : "") << ','
                << (s.chosen ? 1 : 0);
            for (int k = 0; k < kChoiceDims; ++k)
                out << ',' << optionalNumber(s.x[static_cast<std::size_t>(k)], "");
            out << '\n';
        }
    return out ? std::nullopt : std::optional<std::string>("writing " + file.string() + " failed");
}

std::optional<std::string> exportJson(const std::vector<Impression>& impressions, const std::filesystem::path& file)
{
    std::ofstream out(file, std::ios::binary);
    if (!out)
        return "can't write " + file.string();
    out << "{\"dims\": [";
    for (int k = 0; k < kChoiceDims; ++k)
        out << (k > 0 ? ", " : "") << '"' << choiceDimName(k) << '"';
    out << "],\n\"impressions\": [\n";
    for (std::size_t i = 0; i < impressions.size(); ++i)
    {
        const auto& m = impressions[i];
        out << "{\"id\": " << m.id << ", \"session\": " << m.sessionId << ", \"created_at\": " << m.createdAt
            << ", \"context_track\": " << m.contextTrack << ", \"chosen_track\": " << m.chosenTrack << ", \"intent\": \""
            << intentName(m.intent) << "\", \"preset\": " << jsonText(m.preset) << ", \"scope\": " << jsonText(m.scope)
            << ", \"outcome\": \"" << outcomeName(m.outcome)
            << "\", \"chosen_rank\": " << (m.chosenRank ? std::to_string(*m.chosenRank) : "null")
            << ", \"model_version\": " << jsonText(m.modelVersion) << ", \"shown\": " << shownToJson(m.shown) << "}"
            << (i + 1 < impressions.size() ? ",\n" : "\n");
    }
    out << "]}\n";
    return out ? std::nullopt : std::optional<std::string>("writing " + file.string() + " failed");
}

} // namespace devildj::matcher
