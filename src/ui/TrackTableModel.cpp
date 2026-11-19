#include "TrackTableModel.h"

#include "library/Connection.h"
#include "library/SmartRules.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <type_traits>
#include <variant>

namespace devildj::ui
{

namespace
{
juce::String text(const library::Statement& s, int column)
{
    return juce::String::fromUTF8(s.getText(column).c_str());
}

// The SQL behind each column's sort. Empty values go last either way; ties keep import / playlist order.
std::string sortExpression(TrackColumn c)
{
    switch (c)
    {
        case TrackColumn::Title: return "t.title COLLATE NOCASE";
        case TrackColumn::Artist: return "t.artist COLLATE NOCASE";
        case TrackColumn::Album: return "t.album COLLATE NOCASE";
        case TrackColumn::Bpm: return "t.bpm";
        case TrackColumn::Key: return "t.musical_key";
        case TrackColumn::Time: return "t.duration_ms";
        case TrackColumn::Rating: return "nullif(t.rating, 0)";
        case TrackColumn::Colour: return "t.color";
        case TrackColumn::Genre: return "t.genre COLLATE NOCASE";
        case TrackColumn::DateAdded: return "t.date_added";
        case TrackColumn::Comment: return "t.comment COLLATE NOCASE";
        case TrackColumn::Number: break;
    }
    return {};
}

std::string pageSql()
{
    std::string sql = "SELECT id, title, artist, album, genre, comment, musical_key, bpm, duration_ms, rating, color, "
                      "date_added, path FROM tracks WHERE id IN (";
    for (int i = 0; i < TrackTableModel::kPageSize; ++i)
        sql += i == 0 ? "?" : ",?";
    return sql + ")";
}
} // namespace

TrackTableModel::TrackTableModel(library::Connection& reader)
    : db(reader)
{
    reload();
}

juce::String TrackTableModel::keyName(int k)
{
    static const char* const names[] = {"C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    if (k < 0 || k > 23)
        return {};
    return juce::String(names[k % 12]) + (k >= 12 ? "m" : "");
}

juce::String TrackTableModel::timeText(double seconds)
{
    if (seconds <= 0.0)
        return {};
    const int total = static_cast<int>(std::lround(seconds));
    return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
}

void TrackTableModel::setSource(TrackSource s)
{
    if (s == currentSource)
        return;
    currentSource = s;
    reload();
}

void TrackTableModel::setSort(TrackColumn column, bool up)
{
    if (column == sortBy && up == ascending)
        return;
    sortBy = column;
    ascending = up;
    reload();
}

std::string TrackTableModel::orderBy() const
{
    const std::string dir = ascending ? " ASC" : " DESC";
    const std::string natural = currentSource.kind == TrackSource::Kind::Playlist ? "pt.position" : "t.id";
    const auto expr = sortExpression(sortBy);
    if (expr.empty())
        return " ORDER BY " + natural + dir;
    return " ORDER BY (" + expr + ") IS NULL, " + expr + dir + ", " + natural;
}

void TrackTableModel::setFilter(TrackFilter f)
{
    if (f == currentFilter)
        return;
    currentFilter = std::move(f);
    reload();
}

std::string TrackTableModel::ftsQuery(const juce::String& text)
{
    std::string query;
    for (auto word : juce::StringArray::fromTokens(text, " \t\r\n", ""))
    {
        word = word.removeCharacters("\"*"); // FTS5 syntax characters; the rest is fine inside quotes
        bool hasText = false;
        for (auto c : word)
            hasText |= juce::CharacterFunctions::isLetterOrDigit(c);
        if (!hasText)
            continue;
        if (!query.empty())
            query += ' ';
        query += '"' + word.toStdString() + "\"*";
    }
    return query;
}

// FROM + WHERE for the current source and filter; `params` gets the values for its "?" in order.
std::string TrackTableModel::fromWhere(std::vector<library::SqlParam>& params, const TrackFilter& f)
{
    std::string sql;
    std::vector<std::string> where;
    if (currentSource.kind == TrackSource::Kind::Playlist)
    {
        sql = " FROM playlist_tracks pt JOIN tracks t ON t.id = pt.track_id";
        where.push_back("pt.playlist_id = ?");
        params.push_back(currentSource.playlistId);
    }
    else
    {
        sql = " FROM tracks t";
    }
    if (const auto q = ftsQuery(f.text); !q.empty())
    {
        // A join, not "id IN (SELECT ...)": SQLite then walks the full-text matches directly instead of first
        // copying them into a temporary index (that copy doubled the time when a search matched most tracks).
        sql += " JOIN tracks_fts ON tracks_fts.rowid = t.id";
        where.push_back("tracks_fts MATCH ?");
        params.push_back(q);
    }
    if (currentSource.kind == TrackSource::Kind::Smart)
    {
        // P2-07: the playlist's rules, read now, so the list is always current.
        std::string json;
        if (auto* r = db.prepare("SELECT coalesce(rule_json, '') FROM playlists WHERE id = ?"))
        {
            if (r->bind(1, currentSource.playlistId).step())
                json = r->getText(0);
            r->reset();
        }
        const auto rules = json.empty() ? util::Result<library::SmartRules>::ok({}) : library::parseRules(json);
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        const auto condition = rules ? library::toSql(rules.value(), now)
                                     : util::Result<library::SqlCondition>::fail(rules.error());
        if (condition)
        {
            where.push_back(condition.value().sql);
            params.insert(params.end(), condition.value().params.begin(), condition.value().params.end());
        }
        else
        {
            where.push_back("0"); // broken rules show nothing (and say why)
            lastError = juce::String::fromUTF8(condition.error().c_str());
        }
    }
    if (f.bpmMin)
    {
        where.push_back("t.bpm >= ?");
        params.push_back(*f.bpmMin);
    }
    if (f.bpmMax)
    {
        where.push_back("t.bpm <= ?");
        params.push_back(*f.bpmMax);
    }
    const auto list = [](const auto& values)
    {
        std::string s;
        for (const auto v : values)
            s += (s.empty() ? "" : ",") + std::to_string(v);
        return s;
    };
    if (!f.keys.empty())
        where.push_back("t.musical_key IN (" + list(f.keys) + ")");
    if (f.minRating > 0)
        where.push_back("t.rating >= " + std::to_string(f.minRating));
    if (!f.colours.empty())
        where.push_back("t.color IN (" + list(f.colours) + ")");
    if (f.genre.isNotEmpty())
    {
        where.push_back("t.genre = ? COLLATE NOCASE");
        params.push_back(f.genre.toStdString());
    }
    if (!f.tagIds.empty())
        where.push_back("t.id IN (SELECT track_id FROM track_tags WHERE tag_id IN (" + list(f.tagIds) +
                        ") GROUP BY track_id HAVING count(DISTINCT tag_id) = " + std::to_string(f.tagIds.size()) +
                        ")");
    for (std::size_t i = 0; i < where.size(); ++i)
        sql += (i == 0 ? " WHERE " : " AND ") + where[i];
    return sql;
}

namespace
{
void bindAll(library::Statement& s, const std::vector<library::SqlParam>& params)
{
    for (std::size_t i = 0; i < params.size(); ++i)
    {
        const int index = static_cast<int>(i) + 1;
        std::visit(
            [&](const auto& v)
            {
                if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string>)
                    s.bind(index, std::string_view(v));
                else
                    s.bind(index, v);
            },
            params[i]);
    }
}
} // namespace

void TrackTableModel::reload()
{
    const auto started = std::chrono::steady_clock::now();
    ids.clear();
    pages.clear();
    recent.clear();
    lastError.clear();
    std::vector<library::SqlParam> params;
    const auto sql = "SELECT t.id" + fromWhere(params, currentFilter) + orderBy();
    auto* s = db.prepare(sql);
    if (s == nullptr)
    {
        lastError = juce::String::fromUTF8(db.lastError().c_str());
        return;
    }
    bindAll(*s, params);
    while (s->step())
        ids.push_back(s->getInt(0));
    if (!s->ok())
        lastError = juce::String::fromUTF8(s->error().c_str());
    queryMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

int TrackTableModel::unfilteredSize()
{
    if (currentFilter.isEmpty())
        return size();
    std::vector<library::SqlParam> params;
    const auto keepError = lastError;
    auto* s = db.prepare("SELECT count(*)" + fromWhere(params, {}));
    lastError = keepError;
    if (s == nullptr)
        return size();
    bindAll(*s, params);
    const int n = s->step() ? static_cast<int>(s->getInt(0)) : size();
    s->reset();
    return n;
}

int TrackTableModel::countMatching(const library::SmartRules& rules)
{
    const auto condition = library::toSql(
        rules, std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                   .count());
    if (!condition)
        return -1;
    auto* s = db.prepare("SELECT count(*) FROM tracks t WHERE " + condition.value().sql);
    if (s == nullptr)
        return -1;
    bindAll(*s, condition.value().params);
    const int n = s->step() ? static_cast<int>(s->getInt(0)) : -1;
    s->reset();
    return n;
}

std::int64_t TrackTableModel::trackIdAt(int r) const noexcept
{
    return r >= 0 && r < size() ? ids[static_cast<std::size_t>(r)] : 0;
}

int TrackTableModel::rowOfTrack(std::int64_t trackId) const
{
    const auto found = std::find(ids.begin(), ids.end(), trackId);
    return found == ids.end() ? -1 : static_cast<int>(found - ids.begin());
}

const TrackRow* TrackTableModel::row(int index)
{
    if (index < 0 || index >= size())
        return nullptr;
    const auto* p = page(index / kPageSize);
    if (p == nullptr)
        return nullptr;
    const auto& r = (*p)[static_cast<std::size_t>(index % kPageSize)];
    return r.id != 0 ? &r : nullptr;
}

const std::vector<TrackRow>* TrackTableModel::page(int pageIndex)
{
    if (const auto found = pages.find(pageIndex); found != pages.end())
    {
        recent.remove(pageIndex);
        recent.push_front(pageIndex);
        return &found->second;
    }

    const int first = pageIndex * kPageSize;
    const int count = std::min(kPageSize, size() - first);
    if (count <= 0)
        return nullptr;
    auto* s = db.prepare(pageSql());
    if (s == nullptr)
    {
        lastError = juce::String::fromUTF8(db.lastError().c_str());
        return nullptr;
    }
    for (int i = 0; i < kPageSize; ++i)
        s->bind(i + 1, i < count ? ids[static_cast<std::size_t>(first + i)] : std::int64_t{-1});
    std::unordered_map<std::int64_t, TrackRow> byId;
    while (s->step())
    {
        TrackRow r;
        r.id = s->getInt(0);
        r.title = text(*s, 1);
        r.artist = text(*s, 2);
        r.album = text(*s, 3);
        r.genre = text(*s, 4);
        r.comment = text(*s, 5);
        if (const auto k = s->getOptionalInt(6))
            r.key = keyName(static_cast<int>(*k));
        r.bpm = s->getOptionalDouble(7).value_or(0.0);
        r.seconds = static_cast<double>(s->getOptionalInt(8).value_or(0)) / 1000.0;
        r.rating = static_cast<int>(s->getInt(9));
        if (const auto c = s->getOptionalInt(10))
            r.colour = static_cast<std::uint32_t>(*c);
        r.dateAdded = s->getInt(11);
        r.path = text(*s, 12);
        if (r.title.isEmpty()) // untagged: show the file name
            r.title = r.path.fromLastOccurrenceOf("\\", false, false).upToLastOccurrenceOf(".", false, false);
        byId.emplace(r.id, std::move(r));
    }

    std::vector<TrackRow> rows(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        if (const auto found = byId.find(ids[static_cast<std::size_t>(first + i)]); found != byId.end())
            rows[static_cast<std::size_t>(i)] = found->second; // a playlist may show the same track twice
    ++loads;

    if (static_cast<int>(recent.size()) >= kCachedPages)
    {
        pages.erase(recent.back());
        recent.pop_back();
    }
    recent.push_front(pageIndex);
    return &pages.emplace(pageIndex, std::move(rows)).first->second;
}

} // namespace devildj::ui
