#pragma once

// Thread priority helpers. Worker threads started with std::thread / std::async don't inherit their creator's
// priority on Windows (they start at normal), so background jobs that fan out (the loader's resampler and
// decoder helpers) copy the caller's priority into each worker. That keeps track loading below the audio
// thread and the UI even on a laptop with few cores.

namespace devildj::util
{

// Opaque OS priority value of the calling thread (Windows: GetThreadPriority).
int currentThreadPriority() noexcept;
void setCurrentThreadPriority(int priority) noexcept;

// Priorities for tests and helpers (OS values). Background = below normal; Audio = time critical (what
// real audio drivers use for their callback thread).
int backgroundThreadPriority() noexcept;
int audioThreadPriority() noexcept;

} // namespace devildj::util
