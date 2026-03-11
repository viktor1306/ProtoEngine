#pragma once

#include "world/ChunkStorage.hpp"
#include "world/LODController.hpp"
#include "world/ChunkRenderer.hpp"
#include "scene/Frustum.hpp"
#include "core/Math.hpp"
#include <vulkan/vulkan.h>

namespace world {

struct ChunkLifecycleStats {
    uint32_t active         = 0;
    uint32_t placeholders   = 0;
    uint32_t generating     = 0;
    uint32_t ready          = 0;
    uint32_t meshUnassigned = 0;
    uint32_t meshEvicted    = 0;
    uint32_t cachedModified = 0;
};

// ---------------------------------------------------------------------------
// ChunkManager (Facade)
// ---------------------------------------------------------------------------
class ChunkManager {
public:
    explicit ChunkManager(gfx::VulkanContext& context, gfx::GeometryManager& geometryManager,
                          uint32_t meshWorkerThreads = 0);
    ~ChunkManager() = default;

    void generateWorld(int radiusX, int radiusZ, const TerrainConfig& config = {});

    void rebuildDirtyChunks(VkDevice device, float currentTime);

    void cull(VkCommandBuffer cmd, const scene::Frustum& cameraFrustum, const scene::Frustum& shadowFrustum, const core::math::Vec3& cameraPos, float shadowDistanceLimit, float currentTime, uint32_t currentFrame);
    void renderCamera(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t currentFrame);
    void renderShadow(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t currentFrame);

    void markDirty(int cx, int cy, int cz);
    // forceMarkDirty bypasses the isEmpty guard — use when voxel data was actually changed.
    void forceMarkDirty(int cx, int cy, int cz);
    void flushDirty();

    VoxelData getVoxel(int wx, int wy, int wz) const;
    void setVoxel(int wx, int wy, int wz, VoxelData v);

    void updateCamera(const core::math::Vec3& cameraPos, const scene::Frustum& frustum);
    int calculateLOD(int cx, int cy, int cz, int currentLOD = -1) const;

    // Expose LOD configuration directly from LODController
    float& getLodDist0() { return m_lodCtrl.m_lodDist0; }
    float& getLodDist1() { return m_lodCtrl.m_lodDist1; }
    float& getLodHysteresis() { return m_lodCtrl.m_lodHysteresis; }

    int  getRenderRadius()  const { return m_renderRadius; }
    void setRenderRadius(int r)   { m_renderRadius = r; }
    float& getUnloadRadius()      { return m_unloadRadius; }
    float& getFrustumRadius()     { return m_frustumRadius; }
    
    TerrainConfig& getTerrainConfig() { return m_terrainConfig; }

    uint32_t getChunkCount()      const { return static_cast<uint32_t>(m_storage.getChunks().size()); }
    uint32_t getTotalVertices()   const { return m_renderer.getTotalVertices(); }
    uint32_t getTotalIndices()    const { return m_renderer.getTotalIndices(); }
    float    getLastRebuildMs()   const { return m_renderer.getLastRebuildMs(); }
    uint32_t getVisibleCount()    const { return m_renderer.getVisibleCount(); }
    uint32_t getCulledCount()     const { return m_renderer.getCulledCount(); }
    uint32_t getVisibleVertices() const { return m_renderer.getVisibleVertices(); }
    
    uint32_t getWorkerThreads() const { return m_renderer.getWorkerThreads(); }
    int      getPendingMeshes() const { return m_renderer.getPendingMeshes(); }
    ChunkLifecycleStats getLifecycleStats() const;

    const ChunkRenderer& getRenderer() const { return m_renderer; }

    std::array<uint32_t, 3> getLODCounts() const { return m_renderer.getLODCounts(); }

    bool hasMesh() const { return m_renderer.hasMesh(); }

    // Block until all background worker tasks complete (call before first frame to avoid blank screen)
    void waitAllWorkers() { m_renderer.waitAllWorkers(); }

private:
    ChunkStorage  m_storage;
    LODController m_lodCtrl;
    ChunkRenderer m_renderer;

    int   m_renderRadius  = 16;
    float m_unloadRadius  = 512.0f;  // sphere: load+unload distance (blocks)
    float m_frustumRadius = 1024.0f; // frustum: camera view loading distance (blocks)
    
    TerrainConfig m_terrainConfig;
};

} // namespace world
