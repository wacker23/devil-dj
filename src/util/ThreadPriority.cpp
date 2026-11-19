#include "ThreadPriority.h"

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

#if defined(_WIN32)
int currentThreadPriority() noexcept
{
    const int p = GetThreadPriority(GetCurrentThread());
    return p == THREAD_PRIORITY_ERROR_RETURN ? THREAD_PRIORITY_NORMAL : p;
}

void setCurrentThreadPriority(int priority) noexcept
{
    SetThreadPriority(GetCurrentThread(), priority);
}

int backgroundThreadPriority() noexcept
{
    return THREAD_PRIORITY_BELOW_NORMAL;
}

int audioThreadPriority() noexcept
{
    return THREAD_PRIORITY_TIME_CRITICAL;
}

int lowestThreadPriority() noexcept
{
    return THREAD_PRIORITY_LOWEST;
}
#else
int currentThreadPriority() noexcept
{
    return 0;
}
void setCurrentThreadPriority(int) noexcept {}
int backgroundThreadPriority() noexcept
{
    return 0;
}
int audioThreadPriority() noexcept
{
    return 0;
}
int lowestThreadPriority() noexcept
{
    return 0;
}
#endif

} // namespace devildj::util
