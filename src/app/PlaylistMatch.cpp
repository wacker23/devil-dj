#include "app/PlaylistMatch.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace devildj::app
{

namespace
{
// Words that make a version (with whatever comes before them in the brackets: the remixer).
const std::set<std::string> kVersionWords{"remix", "mix", "edit", "dub", "vip", "rework", "bootleg", "version", "extended",
                                          "instrumental", "acapella", "live", "acoustic", "flip", "refix", "rmx", "radio"};
// Bracket / dash parts that say nothing about the recording: dropped.
const std::set<std::string> kNoise{"remaster", "remastered", "mono", "stereo", "explicit", "clean", "original mix", "original"};

std::vector<std::string> words(const std::string& s)
{
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && s[i] == ' ')
            ++i;
        const auto start = i;
        while (i < s.size() && s[i] != ' ')
            ++i;
        if (i > start)
            out.emplace_back(s, start, i - start);
    }
    return out;
}

std::vector<std::string> sortedWords(std::vector<std::string> w)
{
    std::sort(w.begin(), w.end());
    w.erase(std::unique(w.begin(), w.end()), w.end());
    return w;
}

// Words in both of two sorted, unique lists.
int common(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    int n = 0;
    for (std::size_t i = 0, j = 0; i < a.size() && j < b.size();)
    {
        if (a[i] == b[j])
        {
            ++n;
            ++i;
            ++j;
        }
        else if (a[i] < b[j])
            ++i;
        else
            ++j;
    }
    return n;
}

double jaccard(int shared, std::size_t a, std::size_t b)
{
    const auto all = static_cast<double>(a + b) - shared;
    return shared <= 0 || all <= 0.0 ? 0.0 : shared / all;
}

std::vector<std::string> artistWords(const std::vector<std::string>& artists)
{
    std::vector<std::string> w;
    for (const auto& a : artists)
        for (auto& x : words(a))
            w.push_back(std::move(x));
    return sortedWords(std::move(w));
}

bool isNoise(const std::string& part)
{
    if (kNoise.count(part))
        return true;
    for (const auto& w : words(part))
        if (w == "remaster" || w == "remastered")
            return true; // "2011 remaster", "remastered 2009"
    return false;
}

bool isVersion(const std::string& part)
{
    for (const auto& w : words(part))
        if (kVersionWords.count(w))
            return true;
    return false;
}

// "feat. x", "ft x", "featuring x", "with x" at the start of a part: the names after it.
bool featuring(const std::string& part, std::string& names)
{
    for (const char* lead : {"feat ", "ft ", "featuring ", "with "})
        if (part.rfind(lead, 0) == 0)
        {
            names = part.substr(std::string(lead).size());
            return true;
        }
    return false;
}
} // namespace

std::string normalizeName(const std::string& text)
{
    auto s = juce::String::fromUTF8(text.c_str()).toLowerCase();
    // Accents folded (the common Latin ones), "&" and "+" as words, everything else not a letter / digit a space.
    const juce::String from = juce::String::fromUTF8("\xc3\xa0\xc3\xa1\xc3\xa2\xc3\xa3\xc3\xa4\xc3\xa5\xc3\xa8\xc3\xa9\xc3\xaa\xc3\xab\xc3\xac\xc3\xad\xc3\xae\xc3\xaf\xc3\xb2\xc3\xb3\xc3\xb4\xc3\xb5\xc3\xb6\xc3\xb8\xc3\xb9\xc3\xba\xc3\xbb\xc3\xbc\xc3\xb1\xc3\xa7\xc3\xbd\xc3\xbf");
    const juce::String to = "aaaaaaeeeeiiiiooooooouuuuncyy";
    for (int i = 0; i < from.length(); ++i)
        s = s.replaceCharacter(from[i], to[i]);
    s = s.replace("&", " and ").replace("+", " and ").replace("'", "");
    juce::String out;
    for (auto c : s)
        out << (juce::CharacterFunctions::isLetterOrDigit(c) ? c : ' ');
    std::string joined;
    for (const auto& w : words(out.toStdString()))
        joined += (joined.empty() ? "" : " ") + w;
    return joined;
}

std::vector<std::string> splitArtists(const std::string& artists)
{
    std::vector<std::string> out;
    auto s = juce::String::fromUTF8(artists.c_str());
    for (const char* sep : {" feat. ", " feat ", " ft. ", " ft ", " featuring ", " x ", " X ", " vs. ", " vs ", " & ", " and ", ";", "/", ","})
        s = s.replace(sep, "|", true);
    for (const auto& a : juce::StringArray::fromTokens(s, "|", ""))
        if (const auto n = normalizeName(a.toStdString()); !n.empty())
            out.push_back(n);
    return out;
}

TitleParts splitTitle(const std::string& title)
{
    TitleParts t;
    // The parts: what is outside brackets / before " - " is the base; each bracket or dash part is looked at.
    juce::String base, current;
    std::vector<std::string> parts;
    int depth = 0;
    const auto text = juce::String::fromUTF8(title.c_str());
    auto rest = text;
    // " - " splits off a trailing part ("Song - Radio Edit").
    std::string dashPart;
    if (const int dash = text.indexOf(" - "); dash > 0)
    {
        dashPart = normalizeName(text.substring(dash + 3).toStdString());
        rest = text.substring(0, dash);
    }
    for (auto c : rest)
    {
        if (c == '(' || c == '[' || c == '{')
        {
            if (depth++ == 0)
                current.clear();
            continue;
        }
        if ((c == ')' || c == ']' || c == '}') && depth > 0)
        {
            if (--depth == 0)
                parts.push_back(normalizeName(current.toStdString()));
            continue;
        }
        if (depth > 0)
            current += c;
        else
            base += c;
    }
    if (!dashPart.empty())
        parts.push_back(dashPart);
    // Featuring inside the base title ("Song feat. X").
    auto b = normalizeName(base.toStdString());
    for (const char* lead : {" feat ", " ft ", " featuring "})
        if (const auto at = b.find(lead); at != std::string::npos)
        {
            for (const auto& a : splitArtists(b.substr(at + std::string(lead).size())))
                t.featuring.push_back(normalizeName(a));
            b = normalizeName(b.substr(0, at));
        }
    t.base = b;
    std::vector<std::string> versions;
    for (const auto& p : parts)
    {
        std::string names;
        if (p.empty() || isNoise(p))
            continue;
        if (featuring(p, names))
            for (const auto& a : splitArtists(names))
                t.featuring.push_back(normalizeName(a));
        else if (isVersion(p))
            versions.push_back(p);
        else
            t.base = normalizeName(t.base + " " + p); // part of the title itself ("Song (Part 2)")
    }
    t.version = juce::StringArray([&] {
                    juce::StringArray a;
                    for (const auto& v : versions)
                        a.add(juce::String::fromUTF8(v.c_str()));
                    return a;
                }())
                    .joinIntoString(" ")
                    .toStdString();
    if (t.version == "radio edit" || t.version == "radio version")
        t.version.clear(); // the radio edit of a single is the song as most people know it
    return t;
}

struct PlaylistMatcher::Query
{
    TitleParts title;
    std::vector<std::string> artists, titleWords, artistWords;
    double seconds = 0.0;

    explicit Query(const RemoteTrack& r)
        : title(splitTitle(r.title))
        , seconds(r.seconds)
    {
        for (const auto& a : r.artists)
            for (auto& s : splitArtists(a))
                artists.push_back(std::move(s));
        for (const auto& f : title.featuring)
            artists.push_back(f);
        titleWords = sortedWords(words(title.base));
        artistWords = app::artistWords(artists);
    }
};

PlaylistMatcher::PlaylistMatcher(const std::vector<LibraryTrack>& library)
{
    entries.reserve(library.size());
    for (const auto& l : library)
    {
        Entry e;
        e.id = l.id;
        e.title = splitTitle(l.title.empty() ? l.album : l.title);
        e.artists = splitArtists(l.artist);
        for (const auto& f : e.title.featuring)
            e.artists.push_back(f);
        e.seconds = l.seconds;
        e.titleWords = sortedWords(words(e.title.base));
        e.artistWords = artistWords(e.artists);
        const int index = static_cast<int>(entries.size());
        byId[e.id] = index;
        for (const auto& w : e.titleWords)
            byWord[w].push_back(index);
        entries.push_back(std::move(e));
    }
    shared.assign(entries.size(), 0);
}

double PlaylistMatcher::scoreEntry(const Query& q, const Entry& e, double titleOverlap) const
{
    // Title: equal bases, else their word overlap (below 0.5: not this one).
    const double t = q.title.base == e.title.base ? 1.0 : titleOverlap;
    if (t < 0.5)
        return 0.0;
    double s = t;
    // Version: the same (or both plain) is fine; a remix is not the original, nor another remix.
    if (q.title.version != e.title.version)
        s *= q.title.version.empty() || e.title.version.empty() ? 0.6 : 0.75;
    // Artists: one in common, else words in common, else none at all.
    bool same = false;
    for (const auto& a : q.artists)
        same = same || std::find(e.artists.begin(), e.artists.end(), a) != e.artists.end();
    if (!same)
        s *= common(q.artistWords, e.artistWords) > 0 ? 0.85 : (e.artists.empty() ? 0.7 : 0.4);
    // Duration: within 3 s, a little more sure; far apart, less.
    if (q.seconds > 0.0 && e.seconds > 0.0)
    {
        const double d = std::abs(q.seconds - e.seconds);
        s *= d <= 3.0 ? 1.0 : d <= 10.0 ? 0.9 : 0.7;
        if (d <= 3.0)
            s = std::min(1.0, s + 0.05);
    }
    return std::min(1.0, s);
}

double PlaylistMatcher::score(const RemoteTrack& remote, std::int64_t id) const
{
    const auto it = byId.find(id);
    if (it == byId.end())
        return 0.0;
    const Query q(remote);
    const auto& e = entries[static_cast<std::size_t>(it->second)];
    return scoreEntry(q, e, jaccard(common(q.titleWords, e.titleWords), q.titleWords.size(), e.titleWords.size()));
}

TrackMatch PlaylistMatcher::match(const RemoteTrack& remote) const
{
    const Query q(remote);
    // Title words in common with each library title, counted through the index; only those overlapping at least
    // half are scored.
    for (const auto& w : q.titleWords)
        if (const auto it = byWord.find(w); it != byWord.end())
            for (const int i : it->second)
                if (shared[static_cast<std::size_t>(i)]++ == 0)
                    touched.push_back(i);
    std::vector<std::pair<double, int>> byOverlap; // the title overlap (1 for equal bases), entry
    for (const int i : touched)
    {
        const auto& e = entries[static_cast<std::size_t>(i)];
        auto& n = shared[static_cast<std::size_t>(i)];
        const double overlap = q.title.base == e.title.base ? 1.0 : jaccard(n, q.titleWords.size(), e.titleWords.size());
        n = 0;
        if (overlap >= 0.5)
            byOverlap.emplace_back(overlap, i);
    }
    touched.clear();
    // Best overlap first; a score is at most its overlap + 0.05, so once that cannot reach the best score less the
    // margin (nor kMaybe) the rest cannot change the result.
    std::sort(byOverlap.begin(), byOverlap.end(), [](const auto& x, const auto& y) { return x.first > y.first || (x.first == y.first && x.second < y.second); });
    std::vector<std::pair<std::int64_t, double>> scored;
    double best = 0.0;
    for (const auto& [overlap, i] : byOverlap)
    {
        if (overlap + 0.05 < std::max(kMaybe, best - kClearMargin))
            break;
        const auto& e = entries[static_cast<std::size_t>(i)];
        if (const double s = scoreEntry(q, e, overlap); s > 0.0)
        {
            scored.emplace_back(e.id, s);
            best = std::max(best, s);
        }
    }
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.second > b.second || (a.second == b.second && a.first < b.first); });
    TrackMatch m;
    if (scored.empty() || scored.front().second < kMaybe)
        return m;
    m.trackId = scored.front().first;
    m.score = scored.front().second;
    const bool clear = scored.size() == 1 || scored[1].second < m.score - kClearMargin;
    if (m.score >= kSure && clear)
    {
        m.kind = TrackMatch::Kind::Matched;
        return m;
    }
    m.kind = TrackMatch::Kind::Ambiguous;
    for (const auto& c : scored)
        if (c.second >= kMaybe && m.candidates.size() < 5)
            m.candidates.push_back(c);
    return m;
}

} // namespace devildj::app
