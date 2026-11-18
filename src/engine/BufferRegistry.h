#pragma once

#include "engine/EngineIds.h"
#include "engine/TrackBuffer.h"

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace devildj::engine
{

// Owns every TrackBuffer the engine may be using (ARCHITECTURE §3.6). Message thread only.
//
// The engine gets a raw `const TrackBuffer*` plus its id. When the engine stops using a buffer it hands the id
// back (AudioEngine::popReleasedBuffer), and release() drops the shared_ptr here — so the audio thread never
// touches a reference count or frees memory.
class BufferRegistry
{
public:
    BufferId add(std::shared_ptr<const TrackBuffer> buffer)
    {
        const BufferId id = nextId++;
        buffers.emplace(id, std::move(buffer));
        return id;
    }

    // Drops the registry's reference. Returns false if the id is unknown.
    bool release(BufferId id) { return buffers.erase(id) > 0; }

    std::shared_ptr<const TrackBuffer> get(BufferId id) const
    {
        const auto it = buffers.find(id);
        return it != buffers.end() ? it->second : nullptr;
    }

    std::size_t size() const noexcept { return buffers.size(); }

    std::size_t memoryBytes() const noexcept
    {
        std::size_t total = 0;
        for (const auto& [id, buffer] : buffers)
            total += buffer->memoryBytes();
        return total;
    }

private:
    BufferId nextId = 1;
    std::unordered_map<BufferId, std::shared_ptr<const TrackBuffer>> buffers;
};

} // namespace devildj::engine
