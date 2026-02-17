#pragma once

#include "Chunk.hpp"
#include "gfx/resources/GeometryManager.hpp"
#include <vector>
#include <memory>
#include <cstdint>

namespace world {

// Legacy World class — kept for backward compatibility.
// New code should use ChunkManager instead.
// This class now uses the updated Chunk API (32³, VoxelData, VoxelVertex).
class World {
public:
    explicit World(gfx::GeometryManager& geometryManager);

    // Create a simple test world (one chunk with terrain fill).
    void generateTestWorld();

    // Re-generate all chunk meshes and upload to GeometryManager.
    void rebuildMeshes();

    // Draw all chunk meshes.
    void render(VkCommandBuffer cmd);

    // Stats for ImGui
    uint32_t getChunkCount()    const { return static_cast<uint32_t>(m_chunks.size()); }
    uint32_t getTotalVertices() const { return m_totalVertices; }
    uint32_t getTotalIndices()  const { return m_totalIndices; }

    // Direct chunk access
    std::vector<std::unique_ptr<Chunk>>& getChunks() { return m_chunks; }

private:
    gfx::GeometryManager&                    m_geometryManager;
    std::vector<std::unique_ptr<Chunk>>      m_chunks;
    std::vector<std::unique_ptr<gfx::Mesh>>  m_meshes;
    uint32_t m_totalVertices = 0;
    uint32_t m_totalIndices  = 0;
};

} // namespace world
