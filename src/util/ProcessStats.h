#pragma once

// Memory use of this process (P1-20 debug overlay and soak test). Not for the audio thread.

namespace devildj::util
{

struct ProcessMemory
{
    double privateMb = 0.0;    // committed memory that belongs to this process (what a leak grows)
    double workingSetMb = 0.0; // what's currently in RAM
};

ProcessMemory processMemory() noexcept;

} // namespace devildj::util
