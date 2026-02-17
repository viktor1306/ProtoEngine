#pragma once

#include "Chunk.hpp"
#include "gfx/resources/GeometryManager.hpp"
#include <vector>
#include <memory>
#include <cstdint>

namespace world {

// Manages a collection of chunks and their GPU meshes.
// Owns Chunk objects; Mesh pointers are owned by GeometryManager's GPU buffers.
// Call rebuildMeshes() after any chunk data change (or after GeometryManager::reset()).
class World {
public:
    explicit World(gfx::GeometryManager& geometryManager);

    // Create a simple test world (one chunk with terrain fill).
    void generateTestWorld();

    // Re-generate all chunk meshes and upload to GeometryManager.
    // Must be called after GeometryManager::reset() to refresh GPU data.
    void rebuildMeshes();

    // Draw all chunk meshes. Call inside an active render pass.
    void render(VkCommandBuffer cmd);

    // Stats for ImGui
    uint32_t getChunkCount()    const { return static_cast<uint32_t>(m_chunks.size()); }
    uint32_t getTotalVertices() const { return m_totalVertices; }
    uint32_t getTotalIndices()  const { return m_totalIndices; }

    // Direct chunk access (for per-chunk operations like fillRandom)
    std::vector<std::unique_ptr<Chunk>>& getChunks() { return m_chunks; }

private:
    gfx::GeometryManager&                    m_geometryManager;
    std::vector<std::unique_ptr<Chunk>>      m_chunks;
    std::vector<std::unique_ptr<gfx::Mesh>>  m_meshes; // owns Mesh objects
    uint32_t m_totalVertices = 0;
    uint32_t m_totalIndices  = 0;
};

} // namespace world
