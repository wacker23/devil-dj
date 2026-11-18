#pragma once

#include <filesystem>
#include <string>

namespace devildj::library
{

// The tags the deck view needs (P1-13). P2's Importer reads the full set (album, genre, key, artwork, ...).
struct BasicTags
{
    std::string title;  // UTF-8, empty if missing
    std::string artist; // UTF-8, empty if missing
    double bpm = 0.0;   // from the BPM / TBPM tag; 0 = none
};

// Reads tags with TagLib. Never throws; unreadable or untagged files give empty tags. Not for the audio thread.
BasicTags readBasicTags(const std::filesystem::path& file);

// Parses a BPM tag value ("128", "127.98", "128 BPM"); 0 if it isn't a plausible tempo (20..999).
double parseBpmTag(const std::string& text);

} // namespace devildj::library
