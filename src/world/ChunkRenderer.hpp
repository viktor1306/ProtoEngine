#pragma once
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vulkan/vulkan.h>
#include "world/Chunk.hpp"
#include "world/ChunkStorage.hpp"
#include "world/LODController.hpp"
#include "world/MeshWorker.hpp"
#include "gfx/resources/GeometryManager.hpp"
#include "scene/Frustum.hpp"

namespace world {

// Holds the GPU mesh representation for a specific chunk coordinate
struct ChunkRenderData {
    std::unique_ptr<gfx::Mesh> mesh;
    uint32_t vertexCount = 0;
    uint32_t indexCount  = 0;
    scene::AABB aabb;
    bool valid = false;
};

class ChunkRenderer {
public:
    ChunkRenderer(gfx::GeometryManager& geom, ChunkStorage& storage, LODController& lodCtrl, uint32_t meshWorkerThreads = 0);

    // Queue updates
    void markDirty(int cx, int cy, int cz);
    void flushDirty();
    
    // Process async tasks and upload to GPU
    void rebuildDirtyChunks(VkDevice device);
    
    // Draw
    void render(VkCommandBuffer cmd, const scene::Frustum& frustum);

    // LOD
    void setLOD(const IVec3Key& key, int lod);
    int  getLOD(const IVec3Key& key) const;
    std::array<uint32_t, 3> getLODCounts() const;

    // Stats
    uint32_t getTotalVertices() const { return m_totalVertices; }
    uint32_t getTotalIndices()  const { return m_totalIndices; }
    uint32_t getVisibleCount()  const { return m_visibleCount; }
    uint32_t getCulledCount()   const { return m_culledCount; }
    uint32_t getVisibleVertices() const { return m_visibleVertices; }
    float    getLastRebuildMs() const { return m_lastRebuildMs; }
    bool     hasMesh() const;
    uint32_t getWorkerThreads() const { return m_meshWorker.getThreadCount(); }
    int      getPendingMeshes() const { return m_meshWorker.getActiveTasks(); }

    void clear();

private:
    void submitMeshTask(const IVec3Key& key, int lod);
    scene::AABB buildAABB(int cx, int cy, int cz) const;

    gfx::GeometryManager& m_geometryManager;
    ChunkStorage&         m_storage;
    LODController&        m_lodCtrl;
    MeshWorker            m_meshWorker;

    std::unordered_map<IVec3Key, ChunkRenderData, IVec3Hash> m_renderData;
    std::unordered_map<IVec3Key, int, IVec3Hash>             m_chunkLOD;
    std::unordered_set<IVec3Key, IVec3Hash>                  m_dirtyPending;

    uint32_t m_totalVertices   = 0;
    uint32_t m_totalIndices    = 0;
    uint32_t m_visibleCount    = 0;
    uint32_t m_culledCount     = 0;
    uint32_t m_visibleVertices = 0;
    float    m_lastRebuildMs   = 0.0f;
};

} // namespace world
