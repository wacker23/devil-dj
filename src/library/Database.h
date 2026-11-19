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
#include <utility>
#include <vector>

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
    // Receives what one write job changed (Connection::noteChange), after it ran, on the writer thread: keep it
    // short and hand the work to your own thread (the UI posts it to the message thread).
    using ChangeListener = std::function<void(const std::vector<Change>& changes)>;

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
    // Waits until every write queued so far has run (and its change listeners were called).
    void flush();

    // Change notifications (P2-03). Returns an id for removeChangeListener. Thread-safe; a listener may be
    // removed from inside a listener call.
    int addChangeListener(ChangeListener listener);
    void removeChangeListener(int id);

    // A new read-only connection for the calling thread.
    util::Result<std::unique_ptr<Connection>> openReader() const;

    const std::filesystem::path& file() const noexcept { return path; }
    int schemaVersion() const noexcept { return version; }
    bool isWriterThread() const noexcept { return std::this_thread::get_id() == writerThread.get_id(); }

private:
    Database(std::filesystem::path file, std::unique_ptr<Connection> writer, int version);
    void writerLoop();
    void notify(const std::vector<Change>& changes);

    struct Entry
    {
        Job job;
        std::function<void()> done; // after the job and its notifications
    };

    std::filesystem::path path;
    std::unique_ptr<Connection> writer;
    int version = 0;

    std::mutex mutex;
    std::condition_variable wake, idle;
    std::deque<Entry> queue;
    bool busy = false, stopping = false;
    std::thread writerThread;

    std::mutex listenerMutex;
    std::vector<std::pair<int, std::shared_ptr<ChangeListener>>> listeners;
    int nextListenerId = 1;
};

} // namespace devildj::library
