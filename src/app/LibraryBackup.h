#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace devildj::library
{
class Database;
}

namespace devildj::app
{

// Library backup (P2-11): a zip with a consistent copy of the library database (SQLite VACUUM INTO, run on the
// database's writer thread so no write is half in it), the settings file and a short manifest.
//   - Automatic: once a day at start ("auto-YYYYMMDD-HHMMSS.zip" in the backups folder), the newest 7 kept.
//   - By hand: Options > Back up library now... (anywhere; never pruned).
//   - Restore: the zip is checked (a database that opens, passes an integrity check and has a schema this
//     version knows), its files are put next to the live ones as *.restore, and the next start swaps them in
//     before the library opens (the current files are kept as *.before-restore).
namespace backup
{

constexpr int kAutomaticKeep = 7;

// %APPDATA%\Devil-DJ\backups (ARCHITECTURE §8).
juce::File defaultFolder();

// Makes a backup zip. Blocks (call it on a worker thread; the database work runs on its writer thread).
juce::Result create(library::Database& db, const juce::File& settingsFile, const juce::File& zip);

struct Info
{
    juce::File file;
    juce::Time when;
    bool automatic = false;
};
std::vector<Info> list(const juce::File& folder); // newest first

// An automatic backup is due when the newest automatic one is older than 20 hours (or there is none).
bool automaticDue(const juce::File& folder, juce::Time now);
void pruneAutomatic(const juce::File& folder, int keep = kAutomaticKeep);
// create() into the folder as auto-<time>.zip when due, then prune. Returns the zip, or an empty File.
juce::File runAutomatic(library::Database& db, const juce::File& settingsFile, const juce::File& folder,
                        juce::Time now, juce::String* error = nullptr);

// Checks a backup and stages it for the next start. dbFile / settingsFile are the live files.
juce::Result stageRestore(const juce::File& zip, const juce::File& dbFile, const juce::File& settingsFile);
bool restorePending(const juce::File& dbFile);
// Before the library is opened: swaps staged files in. Returns what it did (empty when nothing was staged).
juce::String applyPendingRestore(const juce::File& dbFile, const juce::File& settingsFile);

} // namespace backup
} // namespace devildj::app
