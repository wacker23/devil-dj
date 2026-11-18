#include "ProcessStats.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h first
#include <psapi.h>
#endif

namespace devildj::util
{

ProcessMemory processMemory() noexcept
{
    ProcessMemory m;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)))
    {
        constexpr double mb = 1024.0 * 1024.0;
        m.privateMb = static_cast<double>(counters.PrivateUsage) / mb;
        m.workingSetMb = static_cast<double>(counters.WorkingSetSize) / mb;
    }
#endif
    return m;
}

} // namespace devildj::util
