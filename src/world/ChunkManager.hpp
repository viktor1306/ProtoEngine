#pragma once

#include "Chunk.hpp"
#include "MeshWorker.hpp"
#include "scene/Frustum.hpp"
#include "gfx/resources/GeometryManager.hpp"
#include "gfx/resources/Mesh.hpp"
#include "core/Math.hpp"
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>
#include <array>
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
        size_t h = 2166136261u;
        h ^= static_cast<size_t>(k.x); h *= 16777619u;
        h ^= static_cast<size_t>(k.y); h *= 16777619u;
        h ^= static_cast<size_t>(k.z); h *= 16777619u;
        return h;
    }
};

// ---------------------------------------------------------------------------
// ChunkRenderData — per-chunk GPU sub-allocation info
//
// Each chunk has its own Mesh (sub-allocated from GeometryManager).
// This enables:
//   1. Per-chunk frustum culling (skip vkCmdDrawIndexed for invisible chunks)
//   2. Partial GPU updates (only dirty chunks re-upload, no full reset)
//   3. BDA-ready: firstIndex + vertexOffset passed directly to draw call
// ---------------------------------------------------------------------------
struct ChunkRenderData {
    std::unique_ptr<gfx::Mesh> mesh;
    scene::AABB                aabb;       // world-space AABB for frustum culling
    uint32_t                   vertexCount = 0;
    uint32_t                   indexCount  = 0;
    bool                       valid       = false;
};

// ---------------------------------------------------------------------------
// ChunkManager
//
// Manages a collection of Chunks and their per-chunk GPU meshes.
// Supports:
//   - Frustum culling (per-chunk AABB test before draw)
//   - Async meshing via MeshWorker thread pool
//   - Partial GPU updates (only dirty chunks re-upload)
//   - BDA-compatible per-chunk draw calls
// ---------------------------------------------------------------------------
class ChunkManager {
public:
    explicit ChunkManager(gfx::GeometryManager& geometryManager,
                          uint32_t meshWorkerThreads = 0);
    ~ChunkManager() = default;

    // Generate a flat grid of chunks (radiusX × radiusZ around origin).
    // Submits all chunks to MeshWorker for async meshing.
    // Call rebuildDirtyChunks() after to upload results to GPU.
    void generateWorld(int radiusX, int radiusZ, int seed = 42);

    // Upload completed async mesh results to GPU.
    // Only processes chunks that finished meshing (non-blocking if async).
    // Pass device for vkDeviceWaitIdle on dirty chunks only.
    void rebuildDirtyChunks(VkDevice device);

    // Draw visible chunks using frustum culling.
    // Updates m_visibleCount / m_culledCount stats.
    void render(VkCommandBuffer cmd, const scene::Frustum& frustum);

    // Mark a chunk as dirty (e.g. after block edit).
    // Safe to call multiple times per frame — deduplicates via pending set.
    void markDirty(int cx, int cy, int cz);

    // Flush all pending dirty chunks to MeshWorker (called by setVoxel internally).
    // Exposed so main.cpp can call after a brush operation to batch-submit.
    void flushDirty();

    // ---- Voxel read/write (world integer coords) ---------------------------
    // Returns VOXEL_AIR if coords are outside loaded chunks.
    VoxelData getVoxel(int wx, int wy, int wz) const;

    // Set a voxel and mark the chunk (+ boundary neighbours) dirty.
    // Triggers async re-meshing via MeshWorker.
    void setVoxel(int wx, int wy, int wz, VoxelData v);

    // ---- LOD system ---------------------------------------------------------
    // Update camera position and trigger LOD re-evaluation for all chunks.
    // Call once per frame BEFORE rebuildDirtyChunks().
    // Chunks whose LOD level changed are automatically marked dirty for re-meshing.
    void updateCamera(const core::math::Vec3& cameraPos);

    // Calculate LOD level for a chunk at grid position (cx, cy, cz).
    // Returns 0 (full detail), 1 (half), or 2 (quarter) based on distance.
    int calculateLOD(int cx, int cy, int cz) const;

    // LOD distance thresholds (in world units = blocks).
    // Chunks closer than lodDist0 → LOD 0 (full detail)
    // Chunks between lodDist0 and lodDist1 → LOD 1
    // Chunks farther than lodDist1 → LOD 2
    float m_lodDist0 = 64.0f;   // LOD 0 → LOD 1 boundary
    float m_lodDist1 = 128.0f;  // LOD 1 → LOD 2 boundary

    // ---- Stats (for ImGui) --------------------------------------------------
    uint32_t getChunkCount()      const { return static_cast<uint32_t>(m_chunks.size()); }
    uint32_t getTotalVertices()   const { return m_totalVertices; }
    uint32_t getTotalIndices()    const { return m_totalIndices; }
    float    getLastRebuildMs()   const { return m_lastRebuildMs; }
    uint32_t getVisibleCount()    const { return m_visibleCount; }
    uint32_t getCulledCount()     const { return m_culledCount; }
    uint32_t getVisibleVertices() const { return m_visibleVertices; }
    uint32_t getWorkerThreads()   const { return m_meshWorker.getThreadCount(); }
    int      getPendingMeshes()   const { return m_meshWorker.getActiveTasks(); }

    // LOD stats: count of chunks at each LOD level
    std::array<uint32_t, 3> getLODCounts() const;

    // World-space offset to subtract in the shader (= -bias in world units)
    float getWorldOriginX() const { return -static_cast<float>(m_worldBiasX); }
    float getWorldOriginY() const { return -static_cast<float>(m_worldBiasY); }
    float getWorldOriginZ() const { return -static_cast<float>(m_worldBiasZ); }

    // True if at least one chunk has a valid mesh
    bool hasMesh() const;

private:
    gfx::GeometryManager& m_geometryManager;
    MeshWorker            m_meshWorker;

    std::unordered_map<IVec3Key, std::unique_ptr<Chunk>,       IVec3Hash> m_chunks;
    std::unordered_map<IVec3Key, ChunkRenderData,              IVec3Hash> m_renderData;

    // CPU-side mesh cache: зберігає останні VoxelMeshData для кожного чанку
    // (без world bias — bias застосовується при upload).
    // Потрібен для повного re-upload при GeometryManager::reset().
    std::unordered_map<IVec3Key, VoxelMeshData,                IVec3Hash> m_pendingMeshData;

    // Stats
    uint32_t m_totalVertices   = 0;
    uint32_t m_totalIndices    = 0;
    float    m_lastRebuildMs   = 0.0f;
    uint32_t m_visibleCount    = 0;
    uint32_t m_culledCount     = 0;
    uint32_t m_visibleVertices = 0;

    // World bias: added to vertex coords so all values are non-negative (uint8_t safe).
    int m_worldBiasX = 0;
    int m_worldBiasY = 0;
    int m_worldBiasZ = 0;

    // Get neighbour chunk pointer (nullptr if not loaded)
    const Chunk* getNeighbour(int cx, int cy, int cz) const;

    // Build world-space AABB for a chunk at (cx, cy, cz)
    scene::AABB buildAABB(int cx, int cy, int cz) const;

    // Upload a single chunk's mesh data to GPU
    void uploadChunkMesh(const IVec3Key& key, VoxelMeshData& meshData, VkDevice device);

    // Pending dirty set — deduplicates markDirty calls within a frame.
    // flushDirty() submits all unique entries to MeshWorker and clears the set.
    std::unordered_set<IVec3Key, IVec3Hash> m_dirtyPending;

    // ---- LOD private state --------------------------------------------------
    // Current camera position (updated by updateCamera())
    core::math::Vec3 m_cameraPos{0.0f, 0.0f, 0.0f};

    // Per-chunk current LOD level (0, 1, or 2).
    // -1 = not yet assigned (forces initial meshing with correct LOD).
    std::unordered_map<IVec3Key, int, IVec3Hash> m_chunkLOD;

    // Helper: submit one chunk to MeshWorker with the given LOD
    void submitMeshTask(const IVec3Key& key, int lod);
};

} // namespace world
