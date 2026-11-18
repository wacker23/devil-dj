#include "Database.h"

#include "library/Migrations.h"

namespace devildj::library
{

util::Result<std::unique_ptr<Database>> Database::open(const std::filesystem::path& file)
{
    using R = util::Result<std::unique_ptr<Database>>;
    std::error_code ec;
    if (file.has_parent_path())
        std::filesystem::create_directories(file.parent_path(), ec);

    auto opened = Connection::open(file, Connection::Mode::ReadWrite);
    if (!opened)
        return R::fail(opened.error());
    auto writer = std::move(opened).value();

    // WAL: readers keep reading while the writer writes. NORMAL sync is safe in WAL (a power cut can lose the
    // last commits, never corrupt the file).
    if (writer->queryText("PRAGMA journal_mode = WAL").value_or("") != "wal")
        return R::fail("Can't switch the library to WAL mode: " + writer->lastError());
    if (!writer->exec("PRAGMA synchronous = NORMAL; PRAGMA foreign_keys = ON"))
        return R::fail(writer->lastError());

    auto migrated = migrate(*writer);
    if (!migrated)
        return R::fail(migrated.error());
    return R::ok(std::unique_ptr<Database>(new Database(file, std::move(writer), migrated.value())));
}

Database::Database(std::filesystem::path file, std::unique_ptr<Connection> w, int v)
    : path(std::move(file))
    , writer(std::move(w))
    , version(v)
{
    writerThread = std::thread([this] { writerLoop(); });
}

Database::~Database()
{
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    wake.notify_one();
    writerThread.join();
}

void Database::write(Job job)
{
    {
        std::lock_guard lock(mutex);
        queue.push_back(std::move(job));
    }
    wake.notify_one();
}

void Database::writeAndWait(Job job)
{
    if (isWriterThread()) // called from inside a job: run it in place instead of deadlocking
    {
        job(*writer);
        return;
    }
    std::mutex doneMutex;
    std::condition_variable doneSignal;
    bool done = false;
    write(
        [&](Connection& c)
        {
            job(c);
            std::lock_guard lock(doneMutex);
            done = true;
            doneSignal.notify_one();
        });
    std::unique_lock lock(doneMutex);
    doneSignal.wait(lock, [&] { return done; });
}

void Database::flush()
{
    if (isWriterThread())
        return;
    std::unique_lock lock(mutex);
    idle.wait(lock, [this] { return queue.empty() && !busy; });
}

util::Result<std::unique_ptr<Connection>> Database::openReader() const
{
    return Connection::open(path, Connection::Mode::ReadOnly);
}

void Database::writerLoop()
{
    std::unique_lock lock(mutex);
    for (;;)
    {
        wake.wait(lock, [this] { return stopping || !queue.empty(); });
        if (queue.empty()) // stopping, and every queued write has run
            break;
        auto job = std::move(queue.front());
        queue.pop_front();
        busy = true;
        lock.unlock();
        job(*writer);
        lock.lock();
        busy = false;
        if (queue.empty())
            idle.notify_all();
    }
    idle.notify_all();
}

} // namespace devildj::library
