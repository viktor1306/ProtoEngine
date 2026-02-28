#pragma once
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>
#include "world/Chunk.hpp"
#include "world/ChunkStorage.hpp"
#include "world/LODController.hpp"
#include "world/MeshWorker.hpp"
#include "gfx/resources/GeometryManager.hpp"
#include "gfx/resources/Buffer.hpp"
#include "gfx/core/VulkanContext.hpp"
#include "scene/Frustum.hpp"

namespace world {

// Holds the GPU mesh representation for a specific chunk coordinate
struct ChunkRenderData {
    std::unique_ptr<gfx::Mesh> mesh;
    uint32_t vertexCount = 0;
    uint32_t indexCount  = 0;
    scene::AABB aabb;
    bool valid = false;
    float fadeStartTime = 0.0f;
};

// SSBO layout for chunk instance data
struct alignas(16) ChunkInstanceData {
    float posX, posY, posZ;
    float fadeProgress;
};

class ChunkRenderer {
public:
    static constexpr int MAX_FRAMES_IN_FLIGHT = 3;
    static constexpr uint32_t MAX_VISIBLE_CHUNKS = 8192;

    // Sentinel LOD values:
    //   -1 = chunk exists in storage but LOD not yet assigned
    //   -2 = chunk intentionally evicted (Tier-3), do NOT re-schedule mesh
    static constexpr int LOD_UNASSIGNED = -1;
    static constexpr int LOD_EVICTED    = -2;

    ChunkRenderer(gfx::VulkanContext& context, gfx::GeometryManager& geom, ChunkStorage& storage, LODController& lodCtrl, uint32_t meshWorkerThreads = 0);
    ~ChunkRenderer();

    // Queue updates
    void markDirty(int cx, int cy, int cz);
    void flushDirty();
    void submitGenerateTaskHigh(Chunk* chunk, const TerrainConfig& config);
    void submitGenerateTaskLow (Chunk* chunk, const TerrainConfig& config); // async streaming
    
    // Block until all queued worker tasks are finished (use before first frame)
    void waitAllWorkers();
    
    // Process async tasks and upload to GPU
    void rebuildDirtyChunks(VkDevice device, float currentTime);
    // GPU Compute Frustum Culling and MDI generation
    void cull(VkCommandBuffer cmd, const scene::Frustum& frustum, float currentTime, uint32_t currentFrame);

    // Call inside main render pass
    void render(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t currentFrame);

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

    VkDescriptorSetLayout getDescriptorSetLayout() const { return m_descriptorSetLayout; }

    void clear();
    // Full removal: GPU mesh + removes from m_chunkLOD entirely
    void removeChunk(const IVec3Key& key);
    // Tier-3: frees GPU mesh only, sets LOD_EVICTED in m_chunkLOD
    // so LOD logic won't re-schedule meshing until chunk re-enters view.
    void unloadMeshOnly(const IVec3Key& key);

private:

    scene::AABB buildAABB(int cx, int cy, int cz) const;
    void createDescriptorSetLayout();
    void createBuffers();
    void createComputePipeline();

    // -------------------------------------------------------------
    // Core references
    gfx::VulkanContext&   m_context;
    gfx::GeometryManager& m_geometryManager;
    ChunkStorage&         m_storage;
    LODController&        m_lodCtrl;
    MeshWorker            m_meshWorker;

    // -------------------------------------------------------------
    // Main state
    std::unordered_map<IVec3Key, ChunkRenderData, IVec3Hash> m_renderData;
    std::unordered_map<IVec3Key, int, IVec3Hash>             m_chunkLOD;
    std::unordered_set<IVec3Key, IVec3Hash>                  m_dirtyPending;

    // GPU Culling State (Front-to-Back sorting + Persistent MDI generation)
    struct ChunkDrawCmd {
        IVec3Key key;
        uint32_t poolIndex;
        int lod;
    };
    std::vector<ChunkDrawCmd> m_sortedChunks;
    bool m_listDirty = true;

    // Statistics
    uint32_t m_totalVertices = 0;
    uint32_t m_totalIndices  = 0;
    uint32_t m_visibleCount  = 0;
    uint32_t m_culledCount   = 0;
    uint32_t m_visibleVertices = 0;
    float    m_lastRebuildMs = 0.0f;

    // -------------------------------------------------------------
    // Hardware resources (MDI + SSBO + Compute)
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descriptorPool      = VK_NULL_HANDLE;
    VkDescriptorSet       m_descriptorSets[MAX_FRAMES_IN_FLIGHT]{};

    VkPipelineLayout      m_computePipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_computePipeline       = VK_NULL_HANDLE;

    std::unique_ptr<gfx::Buffer> m_instanceBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<gfx::Buffer> m_indirectBuffers[MAX_FRAMES_IN_FLIGHT];
    void* m_instanceMapped[MAX_FRAMES_IN_FLIGHT]{};
    void* m_indirectMapped[MAX_FRAMES_IN_FLIGHT]{};

    // Tracks how many commands were dispatched per pool
    struct PoolBatch {
        uint32_t poolIndex;
        uint32_t startIdx;
        uint32_t count;
    };
    std::vector<PoolBatch> m_activeBatches;
    uint32_t m_activeInstances = 0;
};

} // namespace world
