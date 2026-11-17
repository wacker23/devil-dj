#pragma once

#include "util/RealtimeGuard.h"

#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace devildj::util
{

enum class LogLevel : std::uint8_t
{
    Debug,
    Info,
    Warning,
    Error,
};

struct LogOptions
{
    std::filesystem::path directory;        // e.g. %LOCALAPPDATA%\Devil-DJ\logs (created if missing)
    std::string baseName = "devil-dj";      // current file: <baseName>.log, older: <baseName>.1.log ...
    std::uintmax_t maxFileBytes = 4u << 20; // rotate when the current file would exceed this
    int maxFiles = 5;                       // current + rotated files kept
};

// Asynchronous file logger. Callers format a line and hand it to a background writer thread, so logging
// never does file I/O on the calling thread. Each start() begins a new file (the previous session's log is
// rotated to <baseName>.1.log). Files rotate by size, keeping at most maxFiles.
//
// Never call from the audio thread: messages logged inside a ScopedRealtimeContext are dropped (and
// counted) before anything is formatted or allocated.
class Log
{
public:
    // Opens the log file and starts the writer thread. Messages logged before start() are kept (bounded)
    // and written once it starts. Returns false if the directory or file can't be created.
    static bool start(const LogOptions& options);

    // Writes everything still queued, stops the writer thread and closes the file. Safe to call twice.
    static void stop();

    static void write(LogLevel level, std::string_view message);

    // Blocks until every message logged so far is on disk.
    static void flush();

    static std::filesystem::path currentFile();
    static std::uint64_t droppedMessages() noexcept;

    // Crash path: appends a line straight to the open file from the calling thread without locking or
    // allocating. Best effort; only for crash handlers.
    static void writeEmergency(std::string_view line) noexcept;
};

namespace detail
{
void countDroppedRealtimeMessage() noexcept;

template <typename... Args>
void logFormatted(LogLevel level, std::format_string<Args...> fmt, Args&&... args)
{
    if (isInRealtimeContext())
    {
        countDroppedRealtimeMessage();
        return;
    }
    Log::write(level, std::format(fmt, std::forward<Args>(args)...));
}
} // namespace detail

template <typename... Args>
void logDebug(std::format_string<Args...> fmt, Args&&... args)
{
    detail::logFormatted(LogLevel::Debug, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void logInfo(std::format_string<Args...> fmt, Args&&... args)
{
    detail::logFormatted(LogLevel::Info, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void logWarning(std::format_string<Args...> fmt, Args&&... args)
{
    detail::logFormatted(LogLevel::Warning, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void logError(std::format_string<Args...> fmt, Args&&... args)
{
    detail::logFormatted(LogLevel::Error, fmt, std::forward<Args>(args)...);
}

} // namespace devildj::util
