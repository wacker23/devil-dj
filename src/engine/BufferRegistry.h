#pragma once

#include "engine/EngineIds.h"
#include "engine/StemBuffer.h"
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

    // A track's stems (P6-04): the same ids and the same release path.
    BufferId addStems(std::shared_ptr<const StemBuffer> stems)
    {
        const BufferId id = nextId++;
        stemBuffers.emplace(id, std::move(stems));
        return id;
    }
    std::shared_ptr<const StemBuffer> getStems(BufferId id) const
    {
        const auto it = stemBuffers.find(id);
        return it != stemBuffers.end() ? it->second : nullptr;
    }

    // Drops the registry's reference. Returns false if the id is unknown.
    bool release(BufferId id) { return buffers.erase(id) > 0 || stemBuffers.erase(id) > 0; }

    // The registry's buffer for a raw pointer (P6-04: the app holds the deck's track while its stems load).
    std::shared_ptr<const TrackBuffer> find(const TrackBuffer* raw) const
    {
        for (const auto& [id, buffer] : buffers)
            if (buffer.get() == raw)
                return buffer;
        return nullptr;
    }

    std::shared_ptr<const TrackBuffer> get(BufferId id) const
    {
        const auto it = buffers.find(id);
        return it != buffers.end() ? it->second : nullptr;
    }

    std::size_t size() const noexcept { return buffers.size() + stemBuffers.size(); }

    std::size_t memoryBytes() const noexcept
    {
        std::size_t total = 0;
        for (const auto& [id, buffer] : buffers)
            total += buffer->memoryBytes();
        for (const auto& [id, stems] : stemBuffers)
            total += stems->memoryBytes();
        return total;
    }

private:
    BufferId nextId = 1;
    std::unordered_map<BufferId, std::shared_ptr<const TrackBuffer>> buffers;
    std::unordered_map<BufferId, std::shared_ptr<const StemBuffer>> stemBuffers;
};

} // namespace devildj::engine
