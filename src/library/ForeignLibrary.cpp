#include "ForeignLibrary.h"

#include "library/Connection.h"
#include "library/ContentHash.h"
#include "library/CueRepository.h"
#include "library/Database.h"
#include "library/PlaylistRepository.h"
#include "library/TagReader.h"
#include "library/TrackRepository.h"
#include "library/XmlReader.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <unordered_map>

namespace devildj::library
{

namespace
{
constexpr std::size_t kMaxNotes = 50;

std::filesystem::path fromUtf8(const std::string& s)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::optional<double> toNumber(const std::string& s)
{
    double v = 0.0;
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string::npos)
        return std::nullopt;
    const auto [p, ec] = std::from_chars(s.data() + first, s.data() + s.size(), v);
    if (ec != std::errc{} || p == s.data() + first)
        return std::nullopt;
    return v;
}

std::optional<std::uint32_t> hexColour(std::string s)
{
    if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0)
        s = s.substr(2);
    else if (!s.empty() && s[0] == '#')
        s = s.substr(1);
    if (s.empty())
        return std::nullopt;
    unsigned v = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (ec != std::errc{} || p != s.data() + s.size())
        return std::nullopt;
    return v & 0xFFFFFFu;
}

// Days since 1970-01-01 for a civil date (proleptic Gregorian).
std::int64_t daysFromCivil(int y, unsigned m, unsigned d)
{
    y -= m <= 2 ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// "2024-05-31" or "2024-05-31T21:30:00Z" -> unix seconds (UTC).
std::optional<std::int64_t> parseDate(const std::string& s)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    const int n = sscanf_s(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se);
    if (n < 3 || y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31)
        return std::nullopt;
    return daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 + h * 3600 + mi * 60 + se;
}

std::string percentDecode(const std::string& s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2])))
        {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        }
        else
        {
            out += s[i];
        }
    }
    return out;
}
} // namespace

std::filesystem::path pathFromLocation(const std::string& location)
{
    std::string s = location;
    for (const char* prefix : {"file://localhost/", "file:///", "file://"})
        if (s.rfind(prefix, 0) == 0)
        {
            s = percentDecode(s.substr(std::string(prefix).size()));
            // file://server/share/x -> \\server\share\x
            if (std::string(prefix) == "file://" && !(s.size() > 1 && s[1] == ':'))
                s = "//" + s;
            break;
        }
    for (auto& c : s)
        if (c == '/')
            c = '\\';
    return fromUtf8(s).lexically_normal();
}

// --- DJ library XML (DJ_PLAYLISTS) -----------------------------------------------------------------------------

util::Result<ForeignLibrary> parseDjPlaylistsXml(std::string_view xml)
{
    using R = util::Result<ForeignLibrary>;
    auto doc = parseXml(xml);
    if (!doc)
        return R::fail(doc.error());
    const auto& root = *doc.value();
    if (root.name != "DJ_PLAYLISTS")
        return R::fail("not a DJ library XML file (no DJ_PLAYLISTS)");
    ForeignLibrary lib;
    lib.format = "DJ library XML";

    std::unordered_map<std::string, std::string> keyByLocation;
    if (const auto* collection = root.child("COLLECTION"))
        for (const auto* t : collection->childrenNamed("TRACK"))
        {
            ForeignTrack track;
            track.key = t->attributeOr("TrackID");
            const auto location = t->attributeOr("Location");
            if (track.key.empty() || location.empty())
                continue;
            track.file = pathFromLocation(location);
            track.title = t->attributeOr("Name");
            track.artist = t->attributeOr("Artist");
            track.album = t->attributeOr("Album");
            track.genre = t->attributeOr("Genre");
            track.comment = t->attributeOr("Comments");
            if (const auto bpm = toNumber(t->attributeOr("AverageBpm")); bpm && *bpm > 0.0)
                track.bpm = *bpm;
            track.tonality = t->attributeOr("Tonality");
            if (const auto r = toNumber(t->attributeOr("Rating")))
                track.rating = std::clamp(static_cast<int>(std::lround(*r / 51.0)), 0, 5); // 0, 51, ... 255
            track.colour = hexColour(t->attributeOr("Colour"));
            if (const auto pc = toNumber(t->attributeOr("PlayCount")))
                track.playCount = std::max(0, static_cast<int>(*pc));
            track.dateAdded = parseDate(t->attributeOr("DateAdded"));
            if (const auto sr = toNumber(t->attributeOr("SampleRate")); sr && *sr > 0.0)
                track.sampleRate = static_cast<int>(*sr);

            for (const auto* tempo : t->childrenNamed("TEMPO"))
            {
                ForeignTempo tm;
                const auto start = toNumber(tempo->attributeOr("Inizio"));
                const auto bpm = toNumber(tempo->attributeOr("Bpm"));
                if (!start || !bpm || *bpm <= 0.0)
                    continue;
                tm.startSeconds = *start;
                tm.bpm = *bpm;
                tm.barBeat = std::clamp(static_cast<int>(toNumber(tempo->attributeOr("Battito")).value_or(1)), 1, 16);
                track.tempos.push_back(tm);
            }
            std::sort(track.tempos.begin(), track.tempos.end(),
                      [](const auto& a, const auto& b) { return a.startSeconds < b.startSeconds; });

            for (const auto* mark : t->childrenNamed("POSITION_MARK"))
            {
                const auto start = toNumber(mark->attributeOr("Start"));
                if (!start || *start < 0.0)
                    continue;
                ForeignCue cue;
                cue.startSeconds = *start;
                cue.endSeconds = toNumber(mark->attributeOr("End")).value_or(0.0);
                cue.label = mark->attributeOr("Name");
                const int type = static_cast<int>(toNumber(mark->attributeOr("Type")).value_or(0));
                const int num = static_cast<int>(toNumber(mark->attributeOr("Num")).value_or(-1));
                if (num >= 0)
                    cue.slot = num;
                cue.kind = type == 4   ? ForeignCue::Kind::Loop
                           : type == 1 ? ForeignCue::Kind::FadeIn
                           : type == 2 ? ForeignCue::Kind::FadeOut
                                       : ForeignCue::Kind::Cue; // 0 cue, 3 load
                if (cue.kind == ForeignCue::Kind::FadeIn || cue.kind == ForeignCue::Kind::FadeOut)
                    cue.slot.reset();
                const auto red = toNumber(mark->attributeOr("Red"));
                const auto green = toNumber(mark->attributeOr("Green"));
                const auto blue = toNumber(mark->attributeOr("Blue"));
                if (red && green && blue)
                    cue.colour = (static_cast<std::uint32_t>(*red) & 0xFF) << 16 |
                                 (static_cast<std::uint32_t>(*green) & 0xFF) << 8 |
                                 (static_cast<std::uint32_t>(*blue) & 0xFF);
                track.cues.push_back(std::move(cue));
            }
            keyByLocation[location] = track.key;
            lib.tracks.push_back(std::move(track));
        }

    std::function<void(const XmlElement&, std::vector<ForeignPlaylist>&)> nodes =
        [&](const XmlElement& parent, std::vector<ForeignPlaylist>& out)
    {
        for (const auto* node : parent.childrenNamed("NODE"))
        {
            ForeignPlaylist p;
            p.name = node->attributeOr("Name");
            p.folder = node->attributeOr("Type") == "0";
            if (p.folder)
                nodes(*node, p.children);
            else
            {
                const bool byLocation = node->attributeOr("KeyType") == "1";
                for (const auto* entry : node->childrenNamed("TRACK"))
                {
                    auto key = entry->attributeOr("Key");
                    if (byLocation)
                    {
                        const auto found = keyByLocation.find(key);
                        if (found == keyByLocation.end())
                            continue;
                        key = found->second;
                    }
                    if (!key.empty())
                        p.trackKeys.push_back(key);
                }
            }
            out.push_back(std::move(p));
        }
    };
    if (const auto* playlists = root.child("PLAYLISTS"))
    {
        // The tree hangs off one ROOT folder node.
        const auto* top = playlists->child("NODE");
        if (top != nullptr)
            nodes(*top, lib.playlists);
    }
    return R::ok(std::move(lib));
}

// --- iTunes / Apple Music library XML (a property list) -------------------------------------------------------

namespace
{
// A plist value: dict / array / string / number / date / bool.
struct Plist
{
    enum class Type
    {
        Dict,
        Array,
        String,
        Number,
        Bool,
        Other
    } type = Type::Other;
    std::string text;
    double number = 0.0;
    bool flag = false;
    std::vector<std::pair<std::string, Plist>> dict; // in file order
    std::vector<Plist> array;

    const Plist* get(std::string_view key) const
    {
        for (const auto& [k, v] : dict)
            if (k == key)
                return &v;
        return nullptr;
    }
    std::string string(std::string_view key) const
    {
        const auto* v = get(key);
        return v != nullptr ? v->text : std::string();
    }
    std::optional<double> num(std::string_view key) const
    {
        const auto* v = get(key);
        if (v == nullptr || v->type != Type::Number)
            return std::nullopt;
        return v->number;
    }
    bool isTrue(std::string_view key) const
    {
        const auto* v = get(key);
        return v != nullptr && v->type == Type::Bool && v->flag;
    }
};

Plist readPlist(const XmlElement& e)
{
    Plist p;
    if (e.name == "dict")
    {
        p.type = Plist::Type::Dict;
        std::string key;
        for (const auto& c : e.children)
        {
            if (c->name == "key")
                key = c->text;
            else
                p.dict.emplace_back(key, readPlist(*c));
        }
    }
    else if (e.name == "array")
    {
        p.type = Plist::Type::Array;
        for (const auto& c : e.children)
            p.array.push_back(readPlist(*c));
    }
    else if (e.name == "integer" || e.name == "real")
    {
        p.type = Plist::Type::Number;
        p.text = e.text;
        p.number = toNumber(e.text).value_or(0.0);
    }
    else if (e.name == "true" || e.name == "false")
    {
        p.type = Plist::Type::Bool;
        p.flag = e.name == "true";
    }
    else
    {
        p.type = Plist::Type::String; // string, date, data
        p.text = e.text;
    }
    return p;
}

std::string numberKey(const Plist* v)
{
    if (v == nullptr)
        return {};
    if (v->type == Plist::Type::Number)
        return std::to_string(static_cast<long long>(v->number));
    return v->text;
}
} // namespace

util::Result<ForeignLibrary> parseITunesXml(std::string_view xml)
{
    using R = util::Result<ForeignLibrary>;
    auto doc = parseXml(xml);
    if (!doc)
        return R::fail(doc.error());
    const auto& root = *doc.value();
    if (root.name != "plist" || root.child("dict") == nullptr)
        return R::fail("not an iTunes library XML file (no plist)");
    const auto top = readPlist(*root.child("dict"));
    ForeignLibrary lib;
    lib.format = "iTunes library XML";

    if (const auto* tracks = top.get("Tracks"))
        for (const auto& [id, t] : tracks->dict)
        {
            const auto location = t.string("Location");
            if (location.rfind("file:", 0) != 0) // streams, cloud-only items
                continue;
            ForeignTrack track;
            track.key = numberKey(t.get("Track ID"));
            if (track.key.empty())
                track.key = id;
            track.file = pathFromLocation(location);
            track.title = t.string("Name");
            track.artist = t.string("Artist");
            track.album = t.string("Album");
            track.genre = t.string("Genre");
            track.comment = t.string("Comments");
            if (const auto bpm = t.num("BPM"); bpm && *bpm > 0.0)
                track.bpm = *bpm;
            // Ratings are 0..100 (20 per star); "Rating Computed" means the album's, not the track's.
            if (const auto r = t.num("Rating"); r && !t.isTrue("Rating Computed"))
                track.rating = std::clamp(static_cast<int>(std::lround(*r / 20.0)), 0, 5);
            if (const auto pc = t.num("Play Count"))
                track.playCount = std::max(0, static_cast<int>(*pc));
            track.dateAdded = parseDate(t.string("Date Added"));
            if (const auto sr = t.num("Sample Rate"); sr && *sr > 0.0)
                track.sampleRate = static_cast<int>(*sr);
            lib.tracks.push_back(std::move(track));
        }

    // Playlists are a flat list; folders link their children with persistent ids.
    struct Flat
    {
        ForeignPlaylist playlist;
        std::string id, parent;
    };
    std::vector<Flat> flat;
    if (const auto* lists = top.get("Playlists"))
        for (const auto& p : lists->array)
        {
            // The whole library, and the built-in Music / Movies / Podcasts ... lists, aren't playlists.
            if (p.isTrue("Master") || p.get("Distinguished Kind") != nullptr || p.string("Visible") == "false")
                continue;
            Flat f;
            f.playlist.name = p.string("Name");
            f.playlist.folder = p.isTrue("Folder");
            f.id = p.string("Playlist Persistent ID");
            f.parent = p.string("Parent Persistent ID");
            if (const auto* items = p.get("Playlist Items"); items != nullptr && !f.playlist.folder)
                for (const auto& item : items->array)
                    if (const auto key = numberKey(item.get("Track ID")); !key.empty())
                        f.playlist.trackKeys.push_back(key);
            flat.push_back(std::move(f));
        }
    // Build the tree (children before their folder is fine: link by id, depth-first from the top).
    std::function<std::vector<ForeignPlaylist>(const std::string&, int)> childrenOf =
        [&](const std::string& parent, int depth)
    {
        std::vector<ForeignPlaylist> out;
        if (depth > 32)
            return out;
        for (const auto& f : flat)
        {
            const bool topLevel = f.parent.empty() ||
                                  std::none_of(flat.begin(), flat.end(), [&](const Flat& o) { return o.id == f.parent; });
            if ((parent.empty() && topLevel) || (!parent.empty() && f.parent == parent))
            {
                auto p = f.playlist;
                if (p.folder && !f.id.empty())
                    p.children = childrenOf(f.id, depth + 1);
                out.push_back(std::move(p));
            }
        }
        return out;
    };
    lib.playlists = childrenOf({}, 0);
    return R::ok(std::move(lib));
}

util::Result<ForeignLibrary> readForeignLibrary(const std::filesystem::path& file)
{
    using R = util::Result<ForeignLibrary>;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return R::fail("Can't open the file");
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    // Pick the parser from the first element name (after the declaration and DOCTYPE).
    std::size_t pos = 0;
    for (;;)
    {
        pos = text.find('<', pos);
        if (pos == std::string::npos)
            return R::fail("not an XML file");
        if (text.compare(pos, 2, "<?") == 0 || text.compare(pos, 2, "<!") == 0)
        {
            ++pos;
            continue;
        }
        break;
    }
    if (text.compare(pos, 13, "<DJ_PLAYLISTS") == 0)
        return parseDjPlaylistsXml(text);
    if (text.compare(pos, 6, "<plist") == 0)
        return parseITunesXml(text);
    return R::fail("neither a DJ library XML nor an iTunes library XML file");
}

// --- applying it ---------------------------------------------------------------------------------------------

namespace
{
std::string anchorsJson(const std::vector<ForeignTempo>& tempos, double sampleRate)
{
    std::string json = "[";
    double beat = 0.0;
    for (std::size_t i = 0; i < tempos.size(); ++i)
    {
        if (i > 0)
            beat += (tempos[i].startSeconds - tempos[i - 1].startSeconds) * tempos[i - 1].bpm / 60.0;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s{\"beat\":%lld,\"sample\":%lld,\"bpm\":%.4f,\"bar_beat\":%d}",
                      i == 0 ? "" : ",", static_cast<long long>(std::llround(beat)),
                      static_cast<long long>(std::llround(tempos[i].startSeconds * sampleRate)), tempos[i].bpm,
                      tempos[i].barBeat);
        json += buf;
    }
    return json + "]";
}
} // namespace

util::Result<ForeignImportResult> importForeignLibrary(Database& db, const ForeignLibrary& foreign,
                                                       const std::string& folderName, const ImportOptions& options)
{
    using R = util::Result<ForeignImportResult>;
    ForeignImportResult result;
    result.tracksInFile = static_cast<int>(foreign.tracks.size());
    const auto note = [&](std::string text)
    {
        if (result.notes.size() < kMaxNotes)
            result.notes.push_back(std::move(text));
    };
    const auto toUtf8 = [](const std::filesystem::path& p)
    {
        const auto u8 = p.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    };

    // 1. The files that exist go through the normal import (tags, cover art, analysis queue).
    std::vector<std::filesystem::path> present;
    std::set<std::string> seen;
    for (const auto& t : foreign.tracks)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(t.file, ec))
        {
            ++result.missingFiles;
            note("missing: " + toUtf8(t.file));
            continue;
        }
        if (seen.insert(libraryPath(t.file)).second)
            present.push_back(t.file);
    }
    if (!present.empty())
        result.added = importTracks(db, present, options).added;

    // 2. Our track id for each of the file's tracks (by path, else the same content elsewhere).
    auto opened = db.openReader();
    if (!opened)
        return R::fail(opened.error());
    auto& reader = *opened.value();
    std::unordered_map<std::string, std::int64_t> idByKey;
    for (const auto& t : foreign.tracks)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(t.file, ec))
            continue;
        std::optional<std::int64_t> id;
        if (auto* s = reader.prepare("SELECT id FROM tracks WHERE path = ? COLLATE NOCASE"))
        {
            if (s->bind(1, libraryPath(t.file)).step())
                id = s->getInt(0);
            s->reset();
        }
        if (!id)
            if (const auto hash = hashFileContent(t.file))
                if (auto* s = reader.prepare("SELECT id FROM tracks WHERE content_hash = ? LIMIT 1"))
                {
                    if (s->bind(1, hash.value().hex).step())
                        id = s->getInt(0);
                    s->reset();
                }
        if (id)
            idByKey[t.key] = *id;
        else
            note("not an audio file the library can read: " + toUtf8(t.file));
    }
    result.matched = static_cast<int>(idByKey.size());

    // 3. Metadata, cues, beatgrids and playlists in one transaction.
    std::string error;
    db.writeAndWait(
        [&](Connection& c)
        {
            const bool ok = c.transaction(
                [&]
                {
                    TrackRepository tracks(c);
                    CueRepository cues(c);
                    for (const auto& t : foreign.tracks)
                    {
                        const auto found = idByKey.find(t.key);
                        if (found == idByKey.end())
                            continue;
                        auto ours = tracks.get(found->second);
                        if (!ours)
                            continue;
                        auto updated = *ours;
                        if (t.rating > 0)
                            updated.rating = t.rating;
                        if (t.colour)
                            updated.color = t.colour;
                        if (t.bpm && !updated.bpmLocked)
                            updated.bpm = t.bpm;
                        if (const auto key = parseKeyTag(t.tonality))
                            updated.musicalKey = key;
                        updated.playCount = std::max(updated.playCount, t.playCount);
                        if (updated.comment.empty())
                            updated.comment = t.comment;
                        if (t.dateAdded && *t.dateAdded > 0)
                            updated.dateAdded = *t.dateAdded;
                        if (!(updated == *ours) && !tracks.update(updated))
                        {
                            error = tracks.error();
                            return false;
                        }

                        const double rate =
                            t.sampleRate.value_or(ours->sampleRate.value_or(44100)) > 0
                                ? static_cast<double>(t.sampleRate.value_or(ours->sampleRate.value_or(44100)))
                                : 44100.0;
                        // Cues only when the track has none yet: importing twice adds nothing.
                        if (!t.cues.empty() && cues.forTrack(found->second).empty())
                            for (const auto& fc : t.cues)
                            {
                                Cue cue;
                                cue.trackId = found->second;
                                cue.startSample = static_cast<std::int64_t>(std::llround(fc.startSeconds * rate));
                                cue.label = fc.label;
                                cue.color = fc.colour;
                                const bool hot = fc.slot && *fc.slot < kHotCueSlots;
                                switch (fc.kind)
                                {
                                    case ForeignCue::Kind::Cue:
                                        cue.kind = hot ? CueKind::HotCue : CueKind::MemoryCue;
                                        break;
                                    case ForeignCue::Kind::Loop:
                                        cue.kind = hot ? CueKind::HotLoop : CueKind::MemoryLoop;
                                        cue.lengthSamples =
                                            static_cast<std::int64_t>(std::llround((fc.endSeconds - fc.startSeconds) * rate));
                                        if (cue.lengthSamples <= 0)
                                        {
                                            note("a loop without an end in " + t.title);
                                            continue;
                                        }
                                        break;
                                    case ForeignCue::Kind::FadeIn: cue.kind = CueKind::MixIn; break;
                                    case ForeignCue::Kind::FadeOut: cue.kind = CueKind::MixOut; break;
                                }
                                if (hot)
                                    cue.slot = *fc.slot;
                                if (cues.add(cue))
                                    ++result.cuesAdded;
                                else
                                    note("cue skipped in " + t.title + ": " + cues.error());
                            }

                        // A beatgrid from the tempo markers when the track has none.
                        if (!t.tempos.empty())
                        {
                            auto* has = c.prepare("SELECT 1 FROM beatgrids WHERE track_id = ?");
                            const bool exists = has != nullptr && has->bind(1, found->second).step();
                            if (has != nullptr)
                                has->reset();
                            if (!exists)
                            {
                                auto* s = c.prepare("INSERT INTO beatgrids (track_id, anchors_json) VALUES (?, ?)");
                                if (s == nullptr || !s->bind(1, found->second).bind(2, anchorsJson(t.tempos, rate)).run())
                                {
                                    error = s != nullptr ? s->error() : c.lastError();
                                    return false;
                                }
                                ++result.beatgridsAdded;
                            }
                        }
                    }

                    // Playlists inside a new folder.
                    PlaylistRepository playlists(c);
                    Playlist folder;
                    folder.name = folderName;
                    folder.kind = PlaylistKind::Folder;
                    const auto created = playlists.create(folder);
                    if (!created)
                    {
                        error = created.error();
                        return false;
                    }
                    result.folderId = created.value();
                    std::function<bool(const std::vector<ForeignPlaylist>&, std::int64_t)> make =
                        [&](const std::vector<ForeignPlaylist>& list, std::int64_t parent)
                    {
                        for (const auto& fp : list)
                        {
                            Playlist p;
                            p.parentId = parent;
                            p.name = fp.name.empty() ? "Untitled" : fp.name;
                            p.kind = fp.folder ? PlaylistKind::Folder : PlaylistKind::Playlist;
                            const auto id = playlists.create(p);
                            if (!id)
                            {
                                error = id.error();
                                return false;
                            }
                            if (fp.folder)
                            {
                                if (!make(fp.children, id.value()))
                                    return false;
                                continue;
                            }
                            std::vector<std::int64_t> ids;
                            for (const auto& key : fp.trackKeys)
                                if (const auto f = idByKey.find(key); f != idByKey.end())
                                    ids.push_back(f->second);
                            if (!playlists.setTracks(id.value(), ids))
                            {
                                error = playlists.error();
                                return false;
                            }
                            ++result.playlists;
                        }
                        return true;
                    };
                    return make(foreign.playlists, result.folderId);
                });
            if (!ok && error.empty())
                error = c.lastError();
        });
    if (!error.empty())
        return R::fail(error);
    return R::ok(std::move(result));
}

} // namespace devildj::library
