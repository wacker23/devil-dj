#pragma once

#include <cstdint>

// RealtimeGuard (P0-09): in Debug builds, the global operator new/delete are replaced with versions that
// report any allocation or deallocation made while the current thread is inside a ScopedRealtimeContext
// (the audio callback). The first violation logs a stack trace; every violation is counted and, with a
// debugger attached, breaks into it.
//
// In Release (DEVILDJ_REALTIME_GUARD not set to 1) everything here compiles to nothing.

#ifndef DEVILDJ_REALTIME_GUARD
#define DEVILDJ_REALTIME_GUARD 0
#endif

namespace devildj::util
{

// What went wrong, passed to the violation handler.
enum class RealtimeViolation : std::uint8_t
{
    Allocation,
    Deallocation,
};

// Called on the offending thread, with the guard temporarily switched off so the handler may allocate.
using RealtimeViolationHandler = void (*)(RealtimeViolation) noexcept;

#if DEVILDJ_REALTIME_GUARD

// Marks the current thread as real-time for the lifetime of the object. Nestable.
class ScopedRealtimeContext
{
public:
    ScopedRealtimeContext() noexcept;
    ~ScopedRealtimeContext() noexcept;

    ScopedRealtimeContext(const ScopedRealtimeContext&) = delete;
    ScopedRealtimeContext& operator=(const ScopedRealtimeContext&) = delete;
};

constexpr bool kRealtimeGuardEnabled = true;

bool isInRealtimeContext() noexcept;

// Total violations seen in this process (all threads).
std::uint64_t realtimeViolationCount() noexcept;

// Replaces the handler; returns the previous one. nullptr restores the default (log once + debug break).
RealtimeViolationHandler setRealtimeViolationHandler(RealtimeViolationHandler handler) noexcept;

#else

class ScopedRealtimeContext
{
public:
    // User-provided (not "= default") so `ScopedRealtimeContext guard;` doesn't trigger C4101 (unused local).
    ScopedRealtimeContext() noexcept {}
    ScopedRealtimeContext(const ScopedRealtimeContext&) = delete;
    ScopedRealtimeContext& operator=(const ScopedRealtimeContext&) = delete;
};

constexpr bool kRealtimeGuardEnabled = false;

inline bool isInRealtimeContext() noexcept
{
    return false;
}
inline std::uint64_t realtimeViolationCount() noexcept
{
    return 0;
}
inline RealtimeViolationHandler setRealtimeViolationHandler(RealtimeViolationHandler) noexcept
{
    return nullptr;
}

#endif

} // namespace devildj::util
