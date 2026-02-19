#pragma once

#include "world/ChunkStorage.hpp"
#include "world/LODController.hpp"
#include "world/ChunkRenderer.hpp"
#include "scene/Frustum.hpp"
#include "core/Math.hpp"
#include <vulkan/vulkan.h>

namespace world {

// ---------------------------------------------------------------------------
// ChunkManager (Facade)
//
// Delegates the heavy lifting to three core modules:
// - ChunkStorage: data & generation
// - LODController: camera distance & level of detail mathematically
// - ChunkRenderer: Async meshing + Vulkan rendering & updating
// ---------------------------------------------------------------------------
class ChunkManager {
public:
    explicit ChunkManager(gfx::GeometryManager& geometryManager,
                          uint32_t meshWorkerThreads = 0);
    ~ChunkManager() = default;

    void generateWorld(int radiusX, int radiusZ, int seed = 42);

    void rebuildDirtyChunks(VkDevice device);
    void render(VkCommandBuffer cmd, const scene::Frustum& frustum);

    void markDirty(int cx, int cy, int cz);
    void flushDirty();

    VoxelData getVoxel(int wx, int wy, int wz) const;
    void setVoxel(int wx, int wy, int wz, VoxelData v);

    void updateCamera(const core::math::Vec3& cameraPos);
    int calculateLOD(int cx, int cy, int cz, int currentLOD = -1) const;

    // Expose LOD configuration directly from LODController
    float& getLodDist0() { return m_lodCtrl.m_lodDist0; }
    float& getLodDist1() { return m_lodCtrl.m_lodDist1; }
    float& getLodHysteresis() { return m_lodCtrl.m_lodHysteresis; }

    uint32_t getChunkCount()      const { return static_cast<uint32_t>(m_storage.getChunks().size()); }
    uint32_t getTotalVertices()   const { return m_renderer.getTotalVertices(); }
    uint32_t getTotalIndices()    const { return m_renderer.getTotalIndices(); }
    float    getLastRebuildMs()   const { return m_renderer.getLastRebuildMs(); }
    uint32_t getVisibleCount()    const { return m_renderer.getVisibleCount(); }
    uint32_t getCulledCount()     const { return m_renderer.getCulledCount(); }
    uint32_t getVisibleVertices() const { return m_renderer.getVisibleVertices(); }
    
    uint32_t getWorkerThreads() const { return m_renderer.getWorkerThreads(); }
    int      getPendingMeshes() const { return m_renderer.getPendingMeshes(); }

    std::array<uint32_t, 3> getLODCounts() const { return m_renderer.getLODCounts(); }

    // World bias access for shading
    float getWorldOriginX() const { return -static_cast<float>(m_storage.getWorldBiasX()); }
    float getWorldOriginY() const { return -static_cast<float>(m_storage.getWorldBiasY()); }
    float getWorldOriginZ() const { return -static_cast<float>(m_storage.getWorldBiasZ()); }

    bool hasMesh() const { return m_renderer.hasMesh(); }

private:
    ChunkStorage  m_storage;
    LODController m_lodCtrl;
    ChunkRenderer m_renderer;
};

} // namespace world
