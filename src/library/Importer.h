#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace devildj::library
{

class Database;

struct ImportOptions
{
    // Lower case with the dot. The app passes engine::supportedTrackExtensions(); the default is the same list
    // (a test keeps them equal).
    std::vector<std::string> extensions = defaultAudioExtensions();
    // Where cover art goes (<content_hash>.jpg / .png ..., recorded in analysis_artifacts as 'artwork'). Empty:
    // artwork isn't stored.
    std::filesystem::path artworkCacheDir;
    int threads = 0;      // file readers; 0 = cores - 1, at most 4
    int batchSize = 100;  // tracks per database transaction
    std::function<void(std::int64_t trackId)> queueAnalysis; // called for every added track
    std::function<void(int done, int total)> onProgress;      // after every batch, on the importing thread
    std::function<bool()> isCancelled;                        // checked between batches

    static std::vector<std::string> defaultAudioExtensions();
};

struct ImportResult
{
    int found = 0;            // audio files in the inputs
    int added = 0;            // new tracks
    int alreadyInLibrary = 0; // same path as a track in the library
    int duplicates = 0;       // same content as a track already in the library (or earlier in this import)
    int relocated = 0;        // same content as a library track whose file is gone: the track now points here (P2-10)
    int failed = 0;           // couldn't be read
    bool cancelled = false;
    double seconds = 0.0;
    std::vector<std::int64_t> addedIds;
    std::vector<std::string> errors; // "path: reason", the first 50
};

// Adds files and folders (searched recursively) to the library (P2-02). For each new audio file: content hash
// (ContentHash.h), tags and audio properties (TagReader.h), cover art, then one row in `tracks` with
// analysis_version 0, written in batches through the database's writer thread; queueAnalysis is called for
// each. Files whose path or content is already in the library are skipped, so importing again adds nothing.
// Blocks until done: call it from a worker thread, never the UI or audio thread. Reader threads inherit the
// caller's priority.
ImportResult importTracks(Database& db, std::span<const std::filesystem::path> filesOrFolders,
                          const ImportOptions& options = {});

// The audio files under the inputs (folders recursively; files as they are), filtered by extension, absolute,
// without repeats, in a stable order. Folders that can't be read are skipped (reported in errors).
std::vector<std::filesystem::path> findAudioFiles(std::span<const std::filesystem::path> filesOrFolders,
                                                  const std::vector<std::string>& extensions,
                                                  std::vector<std::string>* errors = nullptr);

// How a path is stored in tracks.path: absolute, normalized, Windows separators, UTF-8.
std::string libraryPath(const std::filesystem::path& file);

} // namespace devildj::library
