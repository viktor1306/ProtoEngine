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

    static constexpr int LOD_UNASSIGNED = -1;
    static constexpr int LOD_EVICTED    = -2;

    ChunkRenderer(gfx::VulkanContext& context, gfx::GeometryManager& geom, ChunkStorage& storage, LODController& lodCtrl, uint32_t meshWorkerThreads = 0);
    ~ChunkRenderer();

    void markDirty(int cx, int cy, int cz);
    void flushDirty();
    void submitGenerateTaskHigh(Chunk* chunk, const TerrainConfig& config);
    void submitGenerateTaskLow (Chunk* chunk, const TerrainConfig& config);
    
    void waitAllWorkers();
    
    void rebuildDirtyChunks(VkDevice device, float currentTime);
    void cull(VkCommandBuffer cmd, const scene::Frustum& frustum, float currentTime, uint32_t currentFrame);
    void render(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t currentFrame);

    void setLOD(const IVec3Key& key, int lod);
    int  getLOD(const IVec3Key& key) const;
    std::array<uint32_t, 3> getLODCounts() const;

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
    void removeChunk(const IVec3Key& key);
    void unloadMeshOnly(const IVec3Key& key);

private:
    scene::AABB buildAABB(int cx, int cy, int cz) const;
    void createDescriptorSetLayout();
    void createBuffers();
    void createComputePipeline();

    gfx::VulkanContext&   m_context;
    gfx::GeometryManager& m_geometryManager;
    ChunkStorage&         m_storage;
    LODController&        m_lodCtrl;
    MeshWorker            m_meshWorker;

    std::unordered_map<IVec3Key, ChunkRenderData, IVec3Hash> m_renderData;
    std::unordered_map<IVec3Key, int, IVec3Hash> m_chunkLOD;
    std::unordered_set<IVec3Key, IVec3Hash> m_dirtyPending;
    
    struct ChunkDrawCmd {
        IVec3Key key;
        uint32_t poolIndex;
        int lod;
        ChunkRenderData* rd;
    };
    std::vector<ChunkDrawCmd> m_sortedChunks;
    bool m_listDirty = true;
    
    std::vector<ChunkInstanceData> m_cpuInstances;
    std::vector<VkDrawIndexedIndirectCommand> m_cpuIndirects;
    bool m_gpuBuffersDirty[MAX_FRAMES_IN_FLIGHT] = {true, true, true};

    uint32_t m_totalVertices = 0;
    uint32_t m_totalIndices  = 0;
    uint32_t m_visibleCount  = 0;
    uint32_t m_culledCount   = 0;
    uint32_t m_visibleVertices = 0;
    float    m_lastRebuildMs = 0.0f;

    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descriptorPool      = VK_NULL_HANDLE;
    VkDescriptorSet       m_descriptorSets[MAX_FRAMES_IN_FLIGHT]{};

    VkPipelineLayout      m_computePipelineLayout = VK_NULL_HANDLE;
    VkPipeline            m_computePipeline       = VK_NULL_HANDLE;

    std::unique_ptr<gfx::Buffer> m_instanceBuffers[MAX_FRAMES_IN_FLIGHT];
    std::unique_ptr<gfx::Buffer> m_indirectBuffers[MAX_FRAMES_IN_FLIGHT];
    void* m_instanceMapped[MAX_FRAMES_IN_FLIGHT]{};
    void* m_indirectMapped[MAX_FRAMES_IN_FLIGHT]{};

    struct PoolBatch {
        uint32_t poolIndex;
        uint32_t startIdx;
        uint32_t count;
    };
    std::vector<PoolBatch> m_activeBatches;
    uint32_t m_activeInstances = 0;
};

} // namespace world
