#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace devildj::app
{

// The tracklist and cue sheet written next to a mix recording (P3-16), from the session's history: each track
// that started while the file was being recorded, at its time into the file.
struct SheetTrack
{
    double offsetSeconds = 0.0; // into the recording
    std::string artist, title;
};

// History plays (unix seconds) -> the ones inside [fileStart, fileStart + seconds), as offsets. A track already
// playing when the file began counts from 0.
struct PlayedAt
{
    std::int64_t playedAt = 0; // unix seconds
    std::string artist, title;
};
std::vector<SheetTrack> tracksInRecording(const std::vector<PlayedAt>& plays, double fileStartUnix,
                                          double fileSeconds);

// "01. 00:00  Artist - Title" per line.
std::string tracklistText(const std::vector<SheetTrack>& tracks);
// A CUE sheet for `audioFileName` (WAVE / FLAC as given): one TRACK per entry, INDEX 01 mm:ss:ff (75 frames/s).
std::string cueSheet(const std::string& audioFileName, bool flac, const std::vector<SheetTrack>& tracks);

} // namespace devildj::app
