#pragma once

#include "library/Importer.h"
#include "util/Result.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace devildj::library
{

class Database;

// Libraries exported by other software (P2-09), read into one neutral description and then applied to our
// library. Two formats, parsed from their public structure (own code, nothing copied):
//   - DJ library XML (the DJ_PLAYLISTS format other DJ software exports): tracks with cues, loops and tempo
//     markers, a playlist tree.
//   - iTunes / Apple Music "Library.xml" (a property list): tracks, playlists and playlist folders.
struct ForeignCue
{
    enum class Kind
    {
        Cue,  // a memory cue (or a hot cue when slot is set)
        Loop, // a memory loop (or a hot loop when slot is set)
        FadeIn,
        FadeOut,
    };
    Kind kind = Kind::Cue;
    std::optional<int> slot; // hot cue pad, 0-based
    double startSeconds = 0.0, endSeconds = 0.0;
    std::string label;
    std::optional<std::uint32_t> colour; // 0xRRGGBB
};

struct ForeignTempo
{
    double startSeconds = 0.0; // where this tempo starts (a beat)
    double bpm = 0.0;
    int barBeat = 1; // which beat of the bar that is (1..4)
};

struct ForeignTrack
{
    std::string key; // the file's own id for the track (TrackID / Track ID)
    std::filesystem::path file;
    std::string title, artist, album, genre, comment;
    std::optional<double> bpm;
    std::string tonality;             // key as written ("Am", "8A", ...)
    int rating = 0;                   // 0..5 stars
    std::optional<std::uint32_t> colour;
    int playCount = 0;
    std::optional<std::int64_t> dateAdded; // unix seconds
    std::optional<int> sampleRate;          // Hz, when the file says
    std::vector<ForeignCue> cues;
    std::vector<ForeignTempo> tempos;
};

struct ForeignPlaylist
{
    std::string name;
    bool folder = false;
    std::vector<std::string> trackKeys; // ForeignTrack::key, in order (playlists)
    std::vector<ForeignPlaylist> children; // folders
};

struct ForeignLibrary
{
    std::string format; // "DJ library XML" / "iTunes library XML"
    std::vector<ForeignTrack> tracks;
    std::vector<ForeignPlaylist> playlists; // top level
};

util::Result<ForeignLibrary> parseDjPlaylistsXml(std::string_view xml);
util::Result<ForeignLibrary> parseITunesXml(std::string_view xml);
// Reads a file and picks the format from its root element (DJ_PLAYLISTS or plist).
util::Result<ForeignLibrary> readForeignLibrary(const std::filesystem::path& file);

// "file://localhost/C:/My%20Music/a.mp3" -> C:\My Music\a.mp3 (also file:///, plain paths).
std::filesystem::path pathFromLocation(const std::string& location);

struct ForeignImportResult
{
    int tracksInFile = 0;
    int added = 0;          // new in our library
    int matched = 0;        // tracks of the file that are in our library now (added or already there)
    int missingFiles = 0;   // tracks whose file isn't on this computer (skipped)
    int cuesAdded = 0;
    int beatgridsAdded = 0;
    int playlists = 0;      // playlists made (folders not counted)
    std::int64_t folderId = 0; // the folder holding the imported playlists
    std::vector<std::string> notes; // the first problems, for the log
};

// Applies a foreign library: imports the files that exist (importTracks), copies rating, colour, BPM, key,
// play count, an empty comment and the date added; adds cues and a beatgrid to tracks that have none yet (so
// importing again doesn't duplicate them); rebuilds the playlist tree inside a new folder `folderName`.
// Blocks: call it on a worker thread.
util::Result<ForeignImportResult> importForeignLibrary(Database& db, const ForeignLibrary& library,
                                                       const std::string& folderName,
                                                       const ImportOptions& options = {});

} // namespace devildj::library
