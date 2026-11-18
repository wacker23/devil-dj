#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace devildj::library
{

// The tags the deck view needs (P1-13). The importer reads the full set (readTrackTags).
struct BasicTags
{
    std::string title;  // UTF-8, empty if missing
    std::string artist; // UTF-8, empty if missing
    double bpm = 0.0;   // from the BPM / TBPM tag; 0 = none
};

// Reads tags with TagLib. Never throws; unreadable or untagged files give empty tags. Not for the audio thread.
BasicTags readBasicTags(const std::filesystem::path& file);

// Everything the library stores from a file's tags (P2-02). Text is UTF-8; empty / nullopt when missing.
struct TrackTags
{
    bool readable = false; // TagLib could open the file and found audio in it
    std::string title, artist, album, genre, comment;
    std::optional<int> year;
    double bpm = 0.0;                // 0 = no BPM tag
    std::string keyText;             // the key tag as written ("Am", "8A", ...)
    std::optional<int> musicalKey;   // parsed keyText, 0..23 (see parseKeyTag)
    std::optional<int> durationMs;   // audio properties
    std::optional<int> sampleRate;   // Hz
    std::optional<int> bitrateKbps;  // kbit/s
    std::vector<std::uint8_t> artwork; // front cover (or the first picture); empty if none or not asked for
    std::string artworkMimeType;       // "image/jpeg", "image/png", ...
};

TrackTags readTrackTags(const std::filesystem::path& file, bool withArtwork);

// Parses a BPM tag value ("128", "127.98", "128 BPM"); 0 if it isn't a plausible tempo (20..999).
double parseBpmTag(const std::string& text);

// Parses a key tag into the library's key number (ARCHITECTURE §7): 0..11 = C major ... B major by pitch class,
// 12..23 = C minor ... B minor. Accepts note names ("C", "F#m", "Bbmin", "A minor", "Ebmaj", "C#M"), Camelot
// codes ("8A" = A minor, "8B" = C major) and Open Key codes ("1m" = A minor, "1d" = C major). Anything else
// (empty, "o" for off-key, free text) gives nullopt.
std::optional<int> parseKeyTag(const std::string& text);

// File extension for the artwork cache from a MIME type (".jpg", ".png", ".gif", ".bmp", ".webp"), or from the
// image's first bytes when the MIME type is missing or unknown. ".img" if neither tells.
std::string artworkExtension(const std::string& mimeType, const std::vector<std::uint8_t>& data);

} // namespace devildj::library
