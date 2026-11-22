#include "RecordingSheets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace devildj::app
{

namespace
{
std::string twoDigits(int n)
{
    char text[8];
    std::snprintf(text, sizeof(text), "%02d", n);
    return text;
}

// A CUE string: quotes don't fit inside, so they become apostrophes.
std::string quoted(std::string s)
{
    std::replace(s.begin(), s.end(), '"', '\'');
    return "\"" + s + "\"";
}
} // namespace

std::vector<SheetTrack> tracksInRecording(const std::vector<PlayedAt>& plays, double fileStartUnix, double fileSeconds)
{
    std::vector<PlayedAt> sorted = plays;
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.playedAt < b.playedAt; });
    std::vector<SheetTrack> out;
    for (std::size_t i = 0; i < sorted.size(); ++i)
    {
        const double offset = static_cast<double>(sorted[i].playedAt) - fileStartUnix;
        if (offset >= fileSeconds)
            break;
        // Before the file began: only the last one that did (it was playing at its start).
        if (offset < 0.0 && i + 1 < sorted.size() && static_cast<double>(sorted[i + 1].playedAt) - fileStartUnix <= 0.0)
            continue;
        out.push_back({std::max(0.0, offset), sorted[i].artist, sorted[i].title});
    }
    return out;
}

std::string tracklistText(const std::vector<SheetTrack>& tracks)
{
    std::string text;
    int n = 0;
    for (const auto& t : tracks)
    {
        const auto seconds = static_cast<int>(std::floor(t.offsetSeconds));
        const int h = seconds / 3600, m = seconds / 60 % 60, s = seconds % 60;
        text += twoDigits(++n) + ". " + (h > 0 ? std::to_string(h) + ":" : std::string()) + twoDigits(m) + ":" +
                twoDigits(s) + "  " + (t.artist.empty() ? std::string() : t.artist + " - ") + t.title + "\n";
    }
    return text;
}

std::string cueSheet(const std::string& audioFileName, bool flac, const std::vector<SheetTrack>& tracks)
{
    // (CUE readers take FLAC as "WAVE" too: the type names the decoded audio, not the container.)
    (void)flac;
    std::string text = "PERFORMER \"Devil-DJ\"\nTITLE \"Mix\"\nFILE " + quoted(audioFileName) + " WAVE\n";
    int n = 0;
    for (const auto& t : tracks)
    {
        const auto frames = static_cast<long long>(std::llround(t.offsetSeconds * 75.0));
        const int m = static_cast<int>(frames / (75 * 60)), s = static_cast<int>(frames / 75 % 60),
                  f = static_cast<int>(frames % 75);
        text += "  TRACK " + twoDigits(++n) + " AUDIO\n";
        text += "    TITLE " + quoted(t.title) + "\n";
        if (!t.artist.empty())
            text += "    PERFORMER " + quoted(t.artist) + "\n";
        text += "    INDEX 01 " + twoDigits(m) + ":" + twoDigits(s) + ":" + twoDigits(f) + "\n";
    }
    return text;
}

} // namespace devildj::app
