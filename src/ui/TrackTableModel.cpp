#include "TrackTableModel.h"

#include "library/Connection.h"

#include <algorithm>
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

void TrackTableModel::reload()
{
    ids.clear();
    pages.clear();
    recent.clear();
    lastError.clear();
    std::string sql;
    if (currentSource.kind == TrackSource::Kind::Playlist)
        sql = "SELECT t.id FROM playlist_tracks pt JOIN tracks t ON t.id = pt.track_id WHERE pt.playlist_id = ?" +
              orderBy();
    else
        sql = "SELECT t.id FROM tracks t" + orderBy();
    auto* s = db.prepare(sql);
    if (s == nullptr)
    {
        lastError = juce::String::fromUTF8(db.lastError().c_str());
        return;
    }
    if (currentSource.kind == TrackSource::Kind::Playlist)
        s->bind(1, currentSource.playlistId);
    while (s->step())
        ids.push_back(s->getInt(0));
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
