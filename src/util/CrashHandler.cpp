#include "util/CrashHandler.h"

#include "util/Log.h"

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h must come first
#include <dbghelp.h>
#endif

namespace devildj::util
{

#if defined(_WIN32)

namespace
{
// Everything the crash path needs is prepared at install time, so it doesn't allocate while crashing.
wchar_t dumpDirectoryW[MAX_PATH * 2] = {};
volatile LONG crashInProgress = 0;

// Builds "<dir>\devil-dj-crash-YYYYMMDD-HHMMSS-<pid>.dmp" into 'out'. No allocation.
bool buildDumpPath(const wchar_t* directory, wchar_t* out, std::size_t outChars) noexcept
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const int n = std::swprintf(out, outChars, L"%ls\\devil-dj-crash-%04u%02u%02u-%02u%02u%02u-%lu.dmp", directory,
                                t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
    return n > 0 && static_cast<std::size_t>(n) < outChars;
}

bool writeDumpFile(const wchar_t* path, EXCEPTION_POINTERS* exception) noexcept
{
    const HANDLE file =
        CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = exception;
    info.ClientPointers = FALSE;

    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory |
                                                 MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                      exception != nullptr ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
    return ok != FALSE;
}

// Writes the dump and a FATAL log line. 'what' is a short description. No allocation.
void reportCrash(const char* what, EXCEPTION_POINTERS* exception) noexcept
{
    wchar_t path[MAX_PATH * 2 + 64] = {};
    const bool havePath = buildDumpPath(dumpDirectoryW, path, std::size(path));
    const bool dumped = havePath && writeDumpFile(path, exception);

    char pathUtf8[MAX_PATH * 4] = {};
    if (havePath)
        WideCharToMultiByte(CP_UTF8, 0, path, -1, pathUtf8, static_cast<int>(sizeof(pathUtf8)), nullptr, nullptr);

    SYSTEMTIME t{};
    GetLocalTime(&t);
    char line[MAX_PATH * 4 + 256];
    const int n =
        std::snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u [FATAL] [%lu] %s. Minidump: %s\n",
                      t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(),
                      what, dumped ? pathUtf8 : "(failed to write)");
    if (n > 0)
    {
        Log::writeEmergency(line);
        OutputDebugStringA(line);
        std::fputs(line, stderr);
    }
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* exception)
{
    // Only the first crashing thread reports; others wait here until the process ends.
    if (InterlockedCompareExchange(&crashInProgress, 1, 0) != 0)
    {
        Sleep(INFINITE);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    char what[128];
    const auto* record = exception != nullptr ? exception->ExceptionRecord : nullptr;
    std::snprintf(what, sizeof(what), "Unhandled exception 0x%08lX at %p",
                  record != nullptr ? static_cast<unsigned long>(record->ExceptionCode) : 0ul,
                  record != nullptr ? record->ExceptionAddress : nullptr);
    reportCrash(what, exception);
    return EXCEPTION_EXECUTE_HANDLER; // terminate without the Windows error dialog
}

[[noreturn]] void terminateHandler()
{
    if (InterlockedCompareExchange(&crashInProgress, 1, 0) == 0)
        reportCrash("std::terminate called (uncaught C++ exception or noexcept violation)", nullptr);

    TerminateProcess(GetCurrentProcess(), 3);
    std::abort(); // not reached
}
} // namespace

bool installCrashHandler(const CrashHandlerOptions& options)
{
    std::error_code ec;
    std::filesystem::create_directories(options.dumpDirectory, ec);
    if (ec)
        return false;

    const auto dir = std::filesystem::absolute(options.dumpDirectory, ec).wstring();
    if (ec || dir.size() >= std::size(dumpDirectoryW))
        return false;
    wcsncpy_s(dumpDirectoryW, dir.c_str(), _TRUNCATE);

    SetUnhandledExceptionFilter(&unhandledExceptionFilter);
    std::set_terminate(&terminateHandler);
    return true;
}

std::filesystem::path writeMinidump(const std::filesystem::path& directory, void* exceptionPointers)
{
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec)
        return {};

    const auto dir = std::filesystem::absolute(directory, ec).wstring();
    wchar_t path[MAX_PATH * 2 + 64] = {};
    if (ec || !buildDumpPath(dir.c_str(), path, std::size(path)))
        return {};
    if (!writeDumpFile(path, static_cast<EXCEPTION_POINTERS*>(exceptionPointers)))
        return {};
    return std::filesystem::path(path);
}

#else

bool installCrashHandler(const CrashHandlerOptions&)
{
    return false;
}

std::filesystem::path writeMinidump(const std::filesystem::path&, void*)
{
    return {};
}

#endif

void crashForTesting()
{
    volatile int* nowhere = nullptr;
    *nowhere = 0xDEAD;
    std::abort(); // not reached
}

} // namespace devildj::util
