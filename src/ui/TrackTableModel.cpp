#include "TrackTableModel.h"

#include "library/Connection.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

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

// FROM + WHERE for the current source and filter. Parameters, in order: kinds[i] is 't' (next text param),
// 'n' (next number param) or 'p' (the playlist id).
std::string TrackTableModel::fromWhere(std::vector<std::string>& texts, std::vector<double>& numbers,
                                       std::vector<char>& kinds) const
{
    std::string sql;
    std::vector<std::string> where;
    if (currentSource.kind == TrackSource::Kind::Playlist)
    {
        sql = " FROM playlist_tracks pt JOIN tracks t ON t.id = pt.track_id";
        where.push_back("pt.playlist_id = ?");
        kinds.push_back('p');
    }
    else
    {
        sql = " FROM tracks t";
    }
    const auto& f = currentFilter;
    if (const auto q = ftsQuery(f.text); !q.empty())
    {
        // A join, not "id IN (SELECT ...)": SQLite then walks the full-text matches directly instead of first
        // copying them into a temporary index (that copy doubled the time when a search matched most tracks).
        sql += " JOIN tracks_fts ON tracks_fts.rowid = t.id";
        where.push_back("tracks_fts MATCH ?");
        texts.push_back(q);
        kinds.push_back('t');
    }
    if (f.bpmMin)
    {
        where.push_back("t.bpm >= ?");
        numbers.push_back(*f.bpmMin);
        kinds.push_back('n');
    }
    if (f.bpmMax)
    {
        where.push_back("t.bpm <= ?");
        numbers.push_back(*f.bpmMax);
        kinds.push_back('n');
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
        texts.push_back(f.genre.toStdString());
        kinds.push_back('t');
    }
    if (!f.tagIds.empty())
        where.push_back("t.id IN (SELECT track_id FROM track_tags WHERE tag_id IN (" + list(f.tagIds) +
                        ") GROUP BY track_id HAVING count(DISTINCT tag_id) = " + std::to_string(f.tagIds.size()) +
                        ")");
    for (std::size_t i = 0; i < where.size(); ++i)
        sql += (i == 0 ? " WHERE " : " AND ") + where[i];
    return sql;
}

void TrackTableModel::reload()
{
    const auto started = std::chrono::steady_clock::now();
    ids.clear();
    pages.clear();
    recent.clear();
    lastError.clear();
    std::vector<std::string> texts;
    std::vector<double> numbers;
    std::vector<char> kinds;
    const auto sql = "SELECT t.id" + fromWhere(texts, numbers, kinds) + orderBy();
    auto* s = db.prepare(sql);
    if (s == nullptr)
    {
        lastError = juce::String::fromUTF8(db.lastError().c_str());
        return;
    }
    std::size_t ti = 0, ni = 0;
    for (std::size_t i = 0; i < kinds.size(); ++i)
    {
        const int index = static_cast<int>(i) + 1;
        if (kinds[i] == 'p')
            s->bind(index, currentSource.playlistId);
        else if (kinds[i] == 't')
            s->bind(index, std::string_view(texts[ti++]));
        else
            s->bind(index, numbers[ni++]);
    }
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
    std::optional<std::int64_t> n;
    if (currentSource.kind == TrackSource::Kind::Playlist)
    {
        auto* s = db.prepare("SELECT count(*) FROM playlist_tracks WHERE playlist_id = ?");
        if (s != nullptr && s->bind(1, currentSource.playlistId).step())
            n = s->getInt(0);
        if (s != nullptr)
            s->reset();
    }
    else
    {
        n = db.queryInt("SELECT count(*) FROM tracks");
    }
    return static_cast<int>(n.value_or(size()));
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
