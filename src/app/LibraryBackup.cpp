#include "LibraryBackup.h"

#include "library/Connection.h"
#include "library/Database.h"
#include "library/Migrations.h"

#include <algorithm>
#include <filesystem>
#include <optional>

namespace devildj::app::backup
{

namespace
{
constexpr const char* kDbEntry = "library.db";
constexpr const char* kSettingsEntry = "settings.xml";
constexpr const char* kManifestEntry = "manifest.txt";

std::filesystem::path toPath(const juce::File& f)
{
    return std::filesystem::path(f.getFullPathName().toWideCharPointer());
}

juce::File sibling(const juce::File& f, const juce::String& suffix)
{
    return f.getSiblingFile(f.getFileName() + suffix);
}

// "auto-20261120-021500.zip" / "backup-...zip" -> the time in the name (local time).
std::optional<juce::Time> timeFromName(const juce::String& name)
{
    const auto stamp = name.fromFirstOccurrenceOf("-", false, false).upToLastOccurrenceOf(".", false, false);
    if (stamp.length() != 15 || stamp[8] != '-')
        return std::nullopt;
    const auto digits = stamp.removeCharacters("-");
    if (!digits.containsOnly("0123456789"))
        return std::nullopt;
    return juce::Time(digits.substring(0, 4).getIntValue(), digits.substring(4, 6).getIntValue() - 1,
                      digits.substring(6, 8).getIntValue(), digits.substring(8, 10).getIntValue(),
                      digits.substring(10, 12).getIntValue(), digits.substring(12, 14).getIntValue(), 0, true);
}

// Opens a database file read-only and checks it: integrity and a schema we know. Empty string = fine.
juce::String checkDatabase(const juce::File& file)
{
    auto opened = library::Connection::open(toPath(file), library::Connection::Mode::ReadOnly);
    if (!opened)
        return "the library in it can't be opened: " + juce::String::fromUTF8(opened.error().c_str());
    auto& c = *opened.value();
    const int version = library::schemaVersion(c);
    if (version > library::latestSchemaVersion())
        return "it was made by a newer Devil-DJ (library version " + juce::String(version) + ")";
    if (version < 1)
        return "the library in it is empty";
    const auto integrity = c.queryText("PRAGMA integrity_check").value_or("");
    if (integrity != "ok")
        return "the library in it is damaged (" + juce::String::fromUTF8(integrity.c_str()) + ")";
    return {};
}
} // namespace

juce::File defaultFolder()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Devil-DJ/backups");
}

juce::Result create(library::Database& db, const juce::File& settingsFile, const juce::File& zip)
{
    const auto work = juce::File::createTempFile(".backup.db");
    work.deleteFile();
    // A consistent copy, made on the writer thread between writes.
    std::string error;
    const auto target = work.getFullPathName().replace("'", "''").toStdString();
    db.writeAndWait(
        [&](library::Connection& c)
        {
            if (!c.exec("VACUUM INTO '" + target + "'"))
                error = c.lastError();
        });
    if (!error.empty())
        return juce::Result::fail("Can't copy the library: " + juce::String::fromUTF8(error.c_str()));

    juce::ZipFile::Builder builder;
    builder.addFile(work, 9, kDbEntry);
    if (settingsFile.existsAsFile())
        builder.addFile(settingsFile, 9, kSettingsEntry);
    juce::String manifest;
    manifest << "Devil-DJ library backup\n"
             << "made: " << juce::Time::getCurrentTime().toISO8601(true) << "\n"
             << "library version: " << db.schemaVersion() << "\n"
             << "files: " << kDbEntry << (settingsFile.existsAsFile() ? juce::String(", ") + kSettingsEntry : "")
             << "\n";
    builder.addEntry(std::make_unique<juce::MemoryInputStream>(manifest.toRawUTF8(), manifest.getNumBytesAsUTF8(), true), 9,
                     kManifestEntry, juce::Time::getCurrentTime());

    zip.getParentDirectory().createDirectory();
    const auto partial = sibling(zip, ".partial");
    bool written = false;
    {
        juce::FileOutputStream out(partial);
        if (out.openedOk())
        {
            out.setPosition(0);
            out.truncate();
            written = builder.writeToStream(out, nullptr) && out.getStatus().wasOk();
        }
    }
    work.deleteFile();
    if (!written || !partial.moveFileTo(zip)) // only a finished zip gets the real name
    {
        partial.deleteFile();
        return juce::Result::fail("Can't write " + zip.getFullPathName());
    }
    return juce::Result::ok();
}

std::vector<Info> list(const juce::File& folder)
{
    std::vector<Info> out;
    for (const auto& f : folder.findChildFiles(juce::File::findFiles, false, "*.zip"))
    {
        Info i;
        i.file = f;
        i.automatic = f.getFileName().startsWith("auto-");
        i.when = timeFromName(f.getFileName()).value_or(f.getLastModificationTime());
        out.push_back(i);
    }
    std::sort(out.begin(), out.end(), [](const Info& a, const Info& b) { return a.when > b.when; });
    return out;
}

bool automaticDue(const juce::File& folder, juce::Time now)
{
    for (const auto& b : list(folder))
        if (b.automatic)
            return now - b.when >= juce::RelativeTime::hours(20);
    return true;
}

void pruneAutomatic(const juce::File& folder, int keep)
{
    int kept = 0;
    for (const auto& b : list(folder))
        if (b.automatic && ++kept > keep)
            b.file.deleteFile();
}

juce::File runAutomatic(library::Database& db, const juce::File& settingsFile, const juce::File& folder,
                        juce::Time now, juce::String* error)
{
    if (!automaticDue(folder, now))
        return {};
    const auto zip = folder.getChildFile("auto-" + now.formatted("%Y%m%d-%H%M%S") + ".zip");
    const auto result = create(db, settingsFile, zip);
    if (result.failed())
    {
        if (error != nullptr)
            *error = result.getErrorMessage();
        return {};
    }
    pruneAutomatic(folder);
    return zip;
}

juce::Result stageRestore(const juce::File& zipFile, const juce::File& dbFile, const juce::File& settingsFile)
{
    juce::ZipFile zip(zipFile);
    const auto* dbEntry = zip.getEntry(kDbEntry);
    if (zip.getNumEntries() == 0 || dbEntry == nullptr)
        return juce::Result::fail(zipFile.getFileName() + " isn't a Devil-DJ library backup");

    // Unpack next to the live files, then check the library before anything is staged.
    const auto stagedDb = sibling(dbFile, ".restore");
    const auto stagedSettings = sibling(settingsFile, ".restore");
    const auto unpack = [&](const juce::ZipFile::ZipEntry* entry, const juce::File& target)
    {
        std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(*entry));
        if (in == nullptr)
            return false;
        target.getParentDirectory().createDirectory();
        target.deleteFile();
        juce::FileOutputStream out(target);
        if (!out.openedOk() || out.writeFromInputStream(*in, -1) != static_cast<juce::int64>(entry->uncompressedSize))
            return false;
        out.flush();
        return out.getStatus().wasOk();
    };
    if (!unpack(dbEntry, stagedDb))
    {
        stagedDb.deleteFile();
        return juce::Result::fail("Can't unpack the library from " + zipFile.getFileName());
    }
    const auto problem = checkDatabase(stagedDb);
    for (const char* suffix : {"-wal", "-shm"}) // the check may leave the read's side files behind
        sibling(stagedDb, suffix).deleteFile();
    if (problem.isNotEmpty())
    {
        stagedDb.deleteFile();
        return juce::Result::fail("Can't restore " + zipFile.getFileName() + ": " + problem);
    }
    stagedSettings.deleteFile();
    if (const auto* s = zip.getEntry(kSettingsEntry))
        if (!unpack(s, stagedSettings))
        {
            stagedDb.deleteFile();
            stagedSettings.deleteFile();
            return juce::Result::fail("Can't unpack the settings from " + zipFile.getFileName());
        }
    return juce::Result::ok();
}

bool restorePending(const juce::File& dbFile)
{
    return sibling(dbFile, ".restore").existsAsFile();
}

juce::String applyPendingRestore(const juce::File& dbFile, const juce::File& settingsFile)
{
    const auto stagedDb = sibling(dbFile, ".restore");
    if (!stagedDb.existsAsFile())
        return {};
    // Keep what is there now, drop its WAL / SHM (they belong to the old file and would corrupt the new one).
    const auto before = sibling(dbFile, ".before-restore");
    before.deleteFile();
    if (dbFile.existsAsFile() && !dbFile.moveFileTo(before))
        return "Restore failed: can't move the current library aside";
    for (const char* suffix : {"-wal", "-shm"})
        sibling(dbFile, suffix).deleteFile();
    if (!stagedDb.moveFileTo(dbFile))
    {
        before.moveFileTo(dbFile); // put it back
        return "Restore failed: can't put the backup in place";
    }
    juce::String what = "Library restored from backup (the previous one is " + before.getFileName() + ")";
    const auto stagedSettings = sibling(settingsFile, ".restore");
    if (stagedSettings.existsAsFile())
    {
        const auto beforeSettings = sibling(settingsFile, ".before-restore");
        beforeSettings.deleteFile();
        if (settingsFile.existsAsFile())
            settingsFile.moveFileTo(beforeSettings);
        if (stagedSettings.moveFileTo(settingsFile))
            what << "; settings restored too";
    }
    return what;
}

} // namespace devildj::app::backup
