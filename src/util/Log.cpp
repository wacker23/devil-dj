#include "util/Log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace devildj::util
{

namespace
{
constexpr std::size_t kMaxQueuedLines = 10'000;

struct LogState
{
    std::mutex mutex;
    std::condition_variable wake;    // writer: new lines or stop
    std::condition_variable drained; // flush(): queue empty and written
    std::deque<std::string> queue;
    std::uint64_t enqueued = 0; // lines accepted so far
    std::uint64_t written = 0;  // lines written so far (by the writer thread)
    bool running = false;
    bool stopRequested = false;
    std::thread writer;

    LogOptions options;
    std::filesystem::path current;
    std::atomic<std::FILE*> file{nullptr}; // atomic so writeEmergency can read it without the mutex
    std::uintmax_t fileBytes = 0;

    std::atomic<std::uint64_t> dropped{0};
};

LogState& state()
{
    static LogState s;
    return s;
}

const char* levelName(LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO ";
        case LogLevel::Warning:
            return "WARN ";
        case LogLevel::Error:
            return "ERROR";
    }
    return "?????";
}

unsigned long currentThreadId() noexcept
{
#if defined(_WIN32)
    return GetCurrentThreadId();
#else
    return static_cast<unsigned long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

std::string formatLine(LogLevel level, std::string_view message)
{
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif

    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    return std::format("{}.{:03} [{}] [{}] {}\n", stamp, millis, levelName(level), currentThreadId(), message);
}

std::FILE* openForAppend(const std::filesystem::path& path)
{
#if defined(_WIN32)
    // _SH_DENYNO: other programs (and the crash handler) may read the log while we write it.
    return _wfsopen(path.c_str(), L"ab", _SH_DENYNO);
#else
    return std::fopen(path.c_str(), "ab");
#endif
}

std::filesystem::path numberedFile(const LogOptions& o, int index)
{
    if (index == 0)
        return o.directory / (o.baseName + ".log");
    return o.directory / std::format("{}.{}.log", o.baseName, index);
}

// Shifts <base>.log -> <base>.1.log -> ... and drops the oldest. Caller has closed the current file.
void rotateFiles(const LogOptions& o)
{
    std::error_code ec;
    const int keep = o.maxFiles < 1 ? 1 : o.maxFiles;
    std::filesystem::remove(numberedFile(o, keep - 1), ec);
    for (int i = keep - 2; i >= 0; --i)
    {
        const auto from = numberedFile(o, i);
        if (std::filesystem::exists(from, ec))
            std::filesystem::rename(from, numberedFile(o, i + 1), ec);
    }
}

// Writer thread only.
void writeToFile(LogState& s, const std::string& line)
{
    auto* f = s.file.load();
    if (f != nullptr && s.fileBytes > 0 && s.fileBytes + line.size() > s.options.maxFileBytes)
    {
        s.file.store(nullptr);
        std::fclose(f);
        rotateFiles(s.options);
        f = openForAppend(s.current);
        s.file.store(f);
        s.fileBytes = 0;
    }

    if (f != nullptr)
    {
        std::fwrite(line.data(), 1, line.size(), f);
        s.fileBytes += line.size();
    }
}

void writerLoop()
{
    auto& s = state();
    std::deque<std::string> batch;
    for (;;)
    {
        {
            std::unique_lock lock(s.mutex);
            s.wake.wait(lock, [&] { return !s.queue.empty() || s.stopRequested; });
            if (s.queue.empty() && s.stopRequested)
                return;
            batch.swap(s.queue);
        }

        for (const auto& line : batch)
            writeToFile(s, line);
        if (auto* f = s.file.load())
            std::fflush(f);

        {
            std::lock_guard lock(s.mutex);
            s.written += batch.size();
        }
        s.drained.notify_all();
        batch.clear();
    }
}
} // namespace

namespace detail
{
void countDroppedRealtimeMessage() noexcept
{
    state().dropped.fetch_add(1, std::memory_order_relaxed);
}
} // namespace detail

bool Log::start(const LogOptions& options)
{
    auto& s = state();
    stop();

    std::error_code ec;
    std::filesystem::create_directories(options.directory, ec);
    if (ec)
        return false;

    // A new file per session: keep the previous session's log as <base>.1.log.
    const auto current = numberedFile(options, 0);
    if (std::filesystem::exists(current, ec) && std::filesystem::file_size(current, ec) > 0)
        rotateFiles(options);

    auto* f = openForAppend(current);
    if (f == nullptr)
        return false;

    std::lock_guard lock(s.mutex);
    s.options = options;
    s.current = current;
    s.fileBytes = 0;
    s.file.store(f);
    s.stopRequested = false;
    s.running = true;
    s.writer = std::thread(writerLoop);
    if (!s.queue.empty())
        s.wake.notify_one();
    return true;
}

void Log::stop()
{
    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        if (!s.running)
            return;
        s.stopRequested = true;
    }
    s.wake.notify_one();
    s.writer.join();

    std::lock_guard lock(s.mutex);
    s.running = false;
    s.stopRequested = false;
    if (auto* f = s.file.exchange(nullptr))
        std::fclose(f);
    s.drained.notify_all();
}

void Log::write(LogLevel level, std::string_view message)
{
    auto& s = state();
    if (isInRealtimeContext())
    {
        s.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto line = formatLine(level, message);
    {
        std::lock_guard lock(s.mutex);
        if (s.queue.size() >= kMaxQueuedLines)
        {
            s.dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        s.queue.push_back(std::move(line));
        ++s.enqueued;
    }
    s.wake.notify_one();
}

void Log::flush()
{
    auto& s = state();
    std::unique_lock lock(s.mutex);
    if (!s.running)
        return;
    const auto target = s.enqueued;
    s.drained.wait(lock, [&] { return s.written >= target || !s.running; });
}

std::filesystem::path Log::currentFile()
{
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.current;
}

std::uint64_t Log::droppedMessages() noexcept
{
    return state().dropped.load(std::memory_order_relaxed);
}

void Log::writeEmergency(std::string_view line) noexcept
{
    auto* f = state().file.load();
    if (f == nullptr)
        return;
#if defined(_WIN32)
    // No CRT stream lock: the crashed thread might hold it.
    _fwrite_nolock(line.data(), 1, line.size(), f);
    _fflush_nolock(f);
#else
    std::fwrite(line.data(), 1, line.size(), f);
    std::fflush(f);
#endif
}

} // namespace devildj::util
