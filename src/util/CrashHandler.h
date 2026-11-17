#pragma once

#include <filesystem>

namespace devildj::util
{

// Crash reporting (P0-10): on an unhandled exception (access violation etc.) or std::terminate, writes a
// minidump into dumpDirectory, appends a FATAL line to the log, and ends the process.
//
// Windows only; elsewhere these are no-ops that return false / an empty path. With a debugger attached,
// the debugger sees the exception first, as usual.
struct CrashHandlerOptions
{
    std::filesystem::path dumpDirectory; // created if missing; Devil-DJ uses the log folder
};

bool installCrashHandler(const CrashHandlerOptions& options);

// Writes a minidump of the current process now. 'exceptionPointers' is an EXCEPTION_POINTERS* or null.
// Returns the dump path, or an empty path on failure.
std::filesystem::path writeMinidump(const std::filesystem::path& directory, void* exceptionPointers = nullptr);

// Deliberately crashes with an access violation (for the Debug menu's "Crash test").
[[noreturn]] void crashForTesting();

} // namespace devildj::util
