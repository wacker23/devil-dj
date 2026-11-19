#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <list>
#include <optional>
#include <unordered_map>
#include <vector>

namespace devildj::library
{
class Connection;
}

namespace devildj::ui
{

// Columns of the library track table (P2-04). The numbers are the juce::TableHeaderComponent column ids and are
// saved in the column layout setting: never renumber, only add.
enum class TrackColumn
{
    Number = 1, // position in a playlist; import order in the collection
    Title = 2,
    Artist = 3,
    Bpm = 4,
    Key = 5,
    Time = 6,
    Rating = 7,
    Colour = 8,
    Genre = 9,
    DateAdded = 10,
    Comment = 11,
    Album = 12,
};

// What the table shows: the whole collection or one playlist's tracks (smart playlists come in P2-07,
// history in P2-23).
struct TrackSource
{
    enum class Kind
    {
        Collection,
        Playlist,
    };
    Kind kind = Kind::Collection;
    std::int64_t playlistId = 0;
    friend bool operator==(const TrackSource&, const TrackSource&) = default;
};

// Search and filters (P2-05). Everything set must match (AND). Empty / nullopt = no filter.
struct TrackFilter
{
    juce::String text;                    // words matched at the start of words in title / artist / album / comment
    std::optional<double> bpmMin, bpmMax; // inclusive
    std::vector<int> keys;                // any of these (0..23)
    int minRating = 0;                    // at least this many stars
    std::vector<std::uint32_t> colours;   // any of these (0xRRGGBB)
    juce::String genre;                   // this genre (ignoring case)
    std::vector<std::int64_t> tagIds;     // all of these tags

    bool isEmpty() const noexcept
    {
        return text.trim().isEmpty() && !bpmMin && !bpmMax && keys.empty() && minRating <= 0 && colours.empty() &&
               genre.isEmpty() && tagIds.empty();
    }
    friend bool operator==(const TrackFilter&, const TrackFilter&) = default;
};

// One table row, ready to paint (text converted once per page).
struct TrackRow
{
    std::int64_t id = 0;
    juce::String title, artist, album, genre, comment, key, path;
    double bpm = 0.0;     // 0 = unknown
    double seconds = 0.0; // 0 = unknown
    int rating = 0;
    std::optional<std::uint32_t> colour;
    std::int64_t dateAdded = 0;
};

// Data behind the library table (ARCHITECTURE §6: "UI models cache pages of 200 rows"). On a source or sort
// change it reads only the ordered list of track ids (one integer per row, quick even for 50,000 tracks); the
// rows themselves are read a page of 200 at a time when the table first paints them, and the most recent pages
// are kept. reload() after a library change. Message thread only (it owns no thread; the reader connection is
// the UI's).
class TrackTableModel
{
public:
    static constexpr int kPageSize = 200;
    static constexpr int kCachedPages = 32;

    explicit TrackTableModel(library::Connection& reader);

    void setSource(TrackSource source);
    const TrackSource& source() const noexcept { return currentSource; }
    void setSort(TrackColumn column, bool ascending);
    void setFilter(TrackFilter filter);
    const TrackFilter& filter() const noexcept { return currentFilter; }
    int unfilteredSize(); // tracks in the source without the filter (for "12 of 44 tracks")
    double lastQueryMs() const noexcept { return queryMs; }

    // The FTS5 query for a search text: each word as a quoted prefix ("mid"* "dri"*), all required. Empty when
    // the text has no letters or digits.
    static std::string ftsQuery(const juce::String& text);
    TrackColumn sortColumn() const noexcept { return sortBy; }
    bool sortAscending() const noexcept { return ascending; }
    void reload(); // re-reads the ids and drops the cached pages

    int size() const noexcept { return static_cast<int>(ids.size()); }
    std::int64_t trackIdAt(int row) const noexcept;
    int rowOfTrack(std::int64_t trackId) const; // first row showing it, -1 if none
    const TrackRow* row(int index);             // nullptr when out of range or unreadable

    int pageLoads() const noexcept { return loads; } // for tests: how many pages were read
    const juce::String& error() const noexcept { return lastError; }

    static juce::String keyName(int musicalKey);  // 0..23 -> "C", "F#m", ... (empty if out of range)
    static juce::String timeText(double seconds); // "5:07"

private:
    const std::vector<TrackRow>* page(int pageIndex);
    std::string orderBy() const;
    std::string fromWhere(std::vector<std::string>& textParams, std::vector<double>& numberParams,
                          std::vector<char>& kinds) const;

    library::Connection& db;
    TrackSource currentSource;
    TrackColumn sortBy = TrackColumn::Number;
    bool ascending = true;
    TrackFilter currentFilter;
    double queryMs = 0.0;
    std::vector<std::int64_t> ids;
    std::unordered_map<int, std::vector<TrackRow>> pages;
    std::list<int> recent; // most recent first
    int loads = 0;
    juce::String lastError;
};

} // namespace devildj::ui
