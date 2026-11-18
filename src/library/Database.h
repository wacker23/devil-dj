#pragma once

#include "library/Connection.h"
#include "util/Result.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace devildj::library
{

// The library database (ARCHITECTURE §1, §6): SQLite in WAL mode with one writer thread. Every write is a job
// run on that thread, in the order it was queued, so writes never block the caller (the UI) and never race.
// Readers open their own read-only connection (one per thread) and, thanks to WAL, read a consistent snapshot
// while a write is in progress. Statements are prepared once per connection and cached.
class Database
{
public:
    using Job = std::function<void(Connection&)>;

    // Opens (creating if missing) and migrates the file to the latest schema.
    static util::Result<std::unique_ptr<Database>> open(const std::filesystem::path& file);
    ~Database(); // runs the queued writes, then stops the writer thread

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Queues a write; returns at once. The job runs on the writer thread (wrap several statements in
    // Connection::transaction()).
    void write(Job job);
    // Queues a write and waits for it to finish. For worker threads and tests, never the UI thread.
    void writeAndWait(Job job);
    // Waits until every write queued so far has run.
    void flush();

    // A new read-only connection for the calling thread.
    util::Result<std::unique_ptr<Connection>> openReader() const;

    const std::filesystem::path& file() const noexcept { return path; }
    int schemaVersion() const noexcept { return version; }
    bool isWriterThread() const noexcept { return std::this_thread::get_id() == writerThread.get_id(); }

private:
    Database(std::filesystem::path file, std::unique_ptr<Connection> writer, int version);
    void writerLoop();

    std::filesystem::path path;
    std::unique_ptr<Connection> writer;
    int version = 0;

    std::mutex mutex;
    std::condition_variable wake, idle;
    std::deque<Job> queue;
    bool busy = false, stopping = false;
    std::thread writerThread;
};

} // namespace devildj::library
