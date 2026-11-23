#include "app/SpotifyImport.h"

#include "library/Connection.h"
#include "library/PlaylistRepository.h"

#include <set>

namespace devildj::app
{

std::vector<LibraryTrack> loadLibraryTracks(library::Connection& db)
{
    std::vector<LibraryTrack> out;
    auto* st = db.prepare("SELECT id, title, artist, album, duration_ms, path FROM tracks WHERE missing = 0");
    if (st == nullptr)
        return out;
    while (st->step())
    {
        LibraryTrack t;
        t.id = st->getInt(0);
        t.title = st->getText(1);
        t.artist = st->getText(2);
        t.album = st->getText(3);
        t.seconds = static_cast<double>(st->getInt(4)) / 1000.0;
        if (t.title.empty()) // untagged: the file's name
            t.title = juce::File(juce::String::fromUTF8(st->getText(5).c_str())).getFileNameWithoutExtension().toStdString();
        out.push_back(std::move(t));
    }
    st->reset();
    return out;
}

int ImportPlan::matched() const
{
    int n = 0;
    for (const auto& m : matches)
        n += m.kind == TrackMatch::Kind::Matched;
    return n;
}

int ImportPlan::ambiguous() const
{
    int n = 0;
    for (const auto& m : matches)
        n += m.kind == TrackMatch::Kind::Ambiguous;
    return n;
}

int ImportPlan::missing() const
{
    int n = 0;
    for (const auto& m : matches)
        n += m.kind == TrackMatch::Kind::Missing;
    return n;
}

ImportPlan planImport(const std::string& name, std::vector<RemoteTrack> tracks, const PlaylistMatcher& matcher)
{
    ImportPlan p;
    p.name = name;
    p.tracks = std::move(tracks);
    for (const auto& t : p.tracks)
    {
        p.matches.push_back(matcher.match(t));
        p.chosen.push_back(p.matches.back().kind == TrackMatch::Kind::Missing ? 0 : p.matches.back().trackId);
    }
    return p;
}

util::Result<std::int64_t> createImportedPlaylist(library::Connection& db, const ImportPlan& plan)
{
    using R = util::Result<std::int64_t>;
    library::PlaylistRepository repo(db);
    std::set<std::string> taken;
    for (const auto& pl : repo.all())
        taken.insert(pl.name);
    std::string name = plan.name + " (Spotify)";
    for (int n = 2; taken.count(name); ++n)
        name = plan.name + " (Spotify " + std::to_string(n) + ")";
    library::Playlist pl;
    pl.name = name;
    pl.kind = library::PlaylistKind::Playlist;
    auto id = repo.create(pl);
    if (!id)
        return R::fail(id.error());
    std::vector<std::int64_t> ids;
    for (const auto t : plan.chosen)
        if (t != 0)
            ids.push_back(t);
    if (!repo.setTracks(id.value(), ids))
        return R::fail(repo.error());
    return R::ok(id.value());
}

juce::String missingReport(const ImportPlan& plan)
{
    juce::String out;
    for (std::size_t i = 0; i < plan.tracks.size(); ++i)
    {
        if (plan.chosen[i] != 0)
            continue;
        const auto& t = plan.tracks[i];
        juce::StringArray artists;
        for (const auto& a : t.artists)
            artists.add(juce::String::fromUTF8(a.c_str()));
        out << artists.joinIntoString(", ") << " - " << juce::String::fromUTF8(t.title.c_str());
        if (!t.album.empty())
            out << " (" << juce::String::fromUTF8(t.album.c_str()) << ")";
        if (!t.isrc.empty())
            out << " [" << t.isrc << "]";
        out << "\n";
    }
    return out;
}

} // namespace devildj::app
