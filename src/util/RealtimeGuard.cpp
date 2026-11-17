#include "util/RealtimeGuard.h"

#if DEVILDJ_REALTIME_GUARD

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <new>

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

namespace
{
thread_local int realtimeDepth = 0;

std::atomic<std::uint64_t> violationCount{0};
std::atomic<bool> stackTraceLogged{false};
std::atomic<RealtimeViolationHandler> customHandler{nullptr};

void writeDiagnostic(const char* text) noexcept
{
#if defined(_WIN32)
    OutputDebugStringA(text);
#endif
    std::fputs(text, stderr);
}

void logStackTraceOnce(RealtimeViolation what) noexcept
{
    if (stackTraceLogged.exchange(true))
        return;

    char line[512];
    std::snprintf(line, sizeof(line), "RealtimeGuard: %s on a real-time thread. Stack:\n",
                  what == RealtimeViolation::Allocation ? "memory allocation" : "memory deallocation");
    writeDiagnostic(line);

#if defined(_WIN32)
    // Skip logStackTraceOnce, defaultHandler, checkRealtime, allocate/release, operator new/delete: frame 0 = caller.
    constexpr DWORD kGuardFrames = 5;
    void* frames[48] = {};
    const auto count = CaptureStackBackTrace(kGuardFrames, 48, frames, nullptr);

    const HANDLE process = GetCurrentProcess();
    static const bool symbolsReady = SymInitialize(process, nullptr, TRUE) != FALSE;

    alignas(SYMBOL_INFO) char symbolStorage[sizeof(SYMBOL_INFO) + 256] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolStorage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;

    for (USHORT i = 0; i < count; ++i)
    {
        const auto address = reinterpret_cast<DWORD64>(frames[i]);
        DWORD64 displacement = 0;
        if (symbolsReady && SymFromAddr(process, address, &displacement, symbol))
            std::snprintf(line, sizeof(line), "  #%02u %s +0x%llx\n", static_cast<unsigned>(i), symbol->Name,
                          static_cast<unsigned long long>(displacement));
        else
            std::snprintf(line, sizeof(line), "  #%02u 0x%llx\n", static_cast<unsigned>(i),
                          static_cast<unsigned long long>(address));
        writeDiagnostic(line);
    }
#endif
}

void defaultHandler(RealtimeViolation what) noexcept
{
    logStackTraceOnce(what);

#if defined(_WIN32)
    if (IsDebuggerPresent())
        __debugbreak(); // same effect as jassertfalse: stop here when debugging
#endif
}

// Called from every operator new/delete. Cheap when not in a real-time context.
void checkRealtime(RealtimeViolation what) noexcept
{
    if (realtimeDepth <= 0)
        return;

    violationCount.fetch_add(1, std::memory_order_relaxed);

    // Switch the guard off while reporting so the handler (and the stack trace code) may allocate.
    const int savedDepth = realtimeDepth;
    realtimeDepth = 0;

    if (auto* handler = customHandler.load(std::memory_order_acquire))
        handler(what);
    else
        defaultHandler(what);

    realtimeDepth = savedDepth;
}

void* allocate(std::size_t size) noexcept
{
    checkRealtime(RealtimeViolation::Allocation);
    return std::malloc(size != 0 ? size : 1);
}

void* allocateAligned(std::size_t size, std::align_val_t alignment) noexcept
{
    checkRealtime(RealtimeViolation::Allocation);
    return _aligned_malloc(size != 0 ? size : 1, static_cast<std::size_t>(alignment));
}

void release(void* p) noexcept
{
    if (p == nullptr)
        return;
    checkRealtime(RealtimeViolation::Deallocation);
    std::free(p);
}

void releaseAligned(void* p) noexcept
{
    if (p == nullptr)
        return;
    checkRealtime(RealtimeViolation::Deallocation);
    _aligned_free(p);
}
} // namespace

ScopedRealtimeContext::ScopedRealtimeContext() noexcept
{
    ++realtimeDepth;
}

ScopedRealtimeContext::~ScopedRealtimeContext() noexcept
{
    --realtimeDepth;
}

bool isInRealtimeContext() noexcept
{
    return realtimeDepth > 0;
}

std::uint64_t realtimeViolationCount() noexcept
{
    return violationCount.load(std::memory_order_relaxed);
}

RealtimeViolationHandler setRealtimeViolationHandler(RealtimeViolationHandler handler) noexcept
{
    return customHandler.exchange(handler, std::memory_order_acq_rel);
}

} // namespace devildj::util

// ---- Global replacements (Debug only) ----

namespace rt = devildj::util;

void* operator new(std::size_t size)
{
    if (void* p = rt::allocate(size))
        return p;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
    if (void* p = rt::allocate(size))
        return p;
    throw std::bad_alloc();
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    return rt::allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    return rt::allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    if (void* p = rt::allocateAligned(size, alignment))
        return p;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    if (void* p = rt::allocateAligned(size, alignment))
        return p;
    throw std::bad_alloc();
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return rt::allocateAligned(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return rt::allocateAligned(size, alignment);
}

void operator delete(void* p) noexcept
{
    rt::release(p);
}

void operator delete[](void* p) noexcept
{
    rt::release(p);
}

void operator delete(void* p, std::size_t) noexcept
{
    rt::release(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
    rt::release(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept
{
    rt::release(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept
{
    rt::release(p);
}

void operator delete(void* p, std::align_val_t) noexcept
{
    rt::releaseAligned(p);
}

void operator delete[](void* p, std::align_val_t) noexcept
{
    rt::releaseAligned(p);
}

void operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
    rt::releaseAligned(p);
}

void operator delete[](void* p, std::size_t, std::align_val_t) noexcept
{
    rt::releaseAligned(p);
}

void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
    rt::releaseAligned(p);
}

void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
    rt::releaseAligned(p);
}

#endif // DEVILDJ_REALTIME_GUARD
