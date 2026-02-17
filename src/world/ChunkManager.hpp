#pragma once

#include "Chunk.hpp"
#include "gfx/resources/GeometryManager.hpp"
#include "gfx/resources/Mesh.hpp"
#include <unordered_map>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace world {

// ---------------------------------------------------------------------------
// IVec3Key — simple integer 3D key for unordered_map
// ---------------------------------------------------------------------------
struct IVec3Key {
    int x, y, z;
    bool operator==(const IVec3Key& o) const {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct IVec3Hash {
    size_t operator()(const IVec3Key& k) const noexcept {
        // FNV-1a inspired hash
        size_t h = 2166136261u;
        h ^= static_cast<size_t>(k.x); h *= 16777619u;
        h ^= static_cast<size_t>(k.y); h *= 16777619u;
        h ^= static_cast<size_t>(k.z); h *= 16777619u;
        return h;
    }
};

// ---------------------------------------------------------------------------
// ChunkManager
//
// Manages a collection of Chunks and their combined GPU mesh.
// All chunks are merged into ONE draw call via GeometryManager::uploadMeshRaw.
// Chunk world position is encoded in VoxelVertex coordinates:
//   vertex.x/y/z = local coord (0-31)
//   chunkOffset  = passed per-draw via push constants in the voxel pipeline
//
// Since we use a single merged mesh, chunkOffset = (0,0,0) and all vertices
// are pre-offset on the CPU side (local coords + chunk world offset).
// This avoids per-chunk draw calls entirely.
// ---------------------------------------------------------------------------
class ChunkManager {
public:
    explicit ChunkManager(gfx::GeometryManager& geometryManager);
    ~ChunkManager() = default;

    // Generate a flat grid of chunks (radiusX × radiusZ around origin)
    // with heightmap-based terrain. cy=0 only (single vertical layer).
    void generateWorld(int radiusX, int radiusZ, int seed = 42);

    // Rebuild meshes for all dirty chunks and upload to GPU.
    // Calls vkDeviceWaitIdle internally — safe for MAX_FRAMES_IN_FLIGHT=3.
    void rebuildDirtyChunks(VkDevice device);

    // Draw the world mesh (must be called inside an active render pass
    // with the voxel pipeline bound and geometry buffers bound).
    void render(VkCommandBuffer cmd);

    // Mark a chunk as dirty (e.g. after block edit)
    void markDirty(int cx, int cy, int cz);

    // ---- Stats (for ImGui) --------------------------------------------------
    uint32_t getChunkCount()    const { return static_cast<uint32_t>(m_chunks.size()); }
    uint32_t getTotalVertices() const { return m_totalVertices; }
    uint32_t getTotalIndices()  const { return m_totalIndices; }
    float    getLastRebuildMs() const { return m_lastRebuildMs; }
    bool     hasMesh()          const { return m_worldMesh != nullptr; }

private:
    gfx::GeometryManager& m_geometryManager;

    std::unordered_map<IVec3Key, std::unique_ptr<Chunk>, IVec3Hash> m_chunks;

    // Single merged mesh for the entire world
    std::unique_ptr<gfx::Mesh> m_worldMesh;

    // Stats
    uint32_t m_totalVertices = 0;
    uint32_t m_totalIndices  = 0;
    float    m_lastRebuildMs = 0.0f;

    // Get neighbour chunk pointer (nullptr if not loaded)
    const Chunk* getNeighbour(int cx, int cy, int cz) const;
};

} // namespace world
