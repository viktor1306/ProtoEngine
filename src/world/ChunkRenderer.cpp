#include "ChunkRenderer.hpp"
#include "gfx/rendering/Pipeline.hpp"
#include <chrono>
#include <iostream>
#include <fstream>
#include <filesystem>

namespace {

std::vector<char> readBinaryFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("ChunkRenderer: Failed to open shader: " + path);
    }

    const size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    return buffer;
}

VkShaderModule createShaderModule(VkDevice device, const std::vector<char>& code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("ChunkRenderer: Failed to create compute shader module");
    }
    return shaderModule;
}

} // namespace

namespace world {

ChunkRenderer::ChunkRenderer(gfx::VulkanContext& context, gfx::GeometryManager& geom, ChunkStorage& storage, LODController& lodCtrl, uint32_t meshWorkerThreads)
    : m_context(context), m_geometryManager(geom), m_storage(storage), m_lodCtrl(lodCtrl), m_meshWorker(meshWorkerThreads)
{
    std::cout << "[ChunkRenderer] MeshWorker threads: "
              << m_meshWorker.getThreadCount() << "\n" << std::flush;
              
    createDescriptorSetLayouts();
    createBuffers();
    createComputePipeline();
}

ChunkRenderer::~ChunkRenderer() {
    VkDevice device = m_context.getDevice();

    vkDestroyPipeline(device, m_cullPipeline, nullptr);
    vkDestroyPipelineLayout(device, m_cullPipelineLayout, nullptr);
    
    vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, m_cullDescriptorSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, m_descriptorSetLayout, nullptr);
}

void ChunkRenderer::createDescriptorSetLayouts() {
    VkDescriptorSetLayoutBinding bindings[1]{};
    
    // Binding 0: SSBO (InstanceData)
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(m_context.getDevice(), &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("ChunkRenderer: Failed to create SSBO descriptor set layout!");
    }

    VkDescriptorSetLayoutBinding cullBindings[2]{};
    cullBindings[0].binding = 0;
    cullBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    cullBindings[0].descriptorCount = 1;
    cullBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    cullBindings[1].binding = 1;
    cullBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    cullBindings[1].descriptorCount = 1;
    cullBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo cullLayoutInfo{};
    cullLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    cullLayoutInfo.bindingCount = 2;
    cullLayoutInfo.pBindings = cullBindings;

    if (vkCreateDescriptorSetLayout(m_context.getDevice(), &cullLayoutInfo, nullptr, &m_cullDescriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("ChunkRenderer: Failed to create cull descriptor set layout!");
    }

    VkDescriptorPoolSize poolSizes[1]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT * 6;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT * 4;

    if (vkCreateDescriptorPool(m_context.getDevice(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("ChunkRenderer: Failed to create SSBO descriptor pool!");
    }

    std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, m_descriptorSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    
    allocInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(m_context.getDevice(), &allocInfo, m_cameraDescriptorSets) != VK_SUCCESS) {
        throw std::runtime_error("ChunkRenderer: Failed to allocate Camera SSBO descriptor sets!");
    }
    if (vkAllocateDescriptorSets(m_context.getDevice(), &allocInfo, m_shadowDescriptorSets) != VK_SUCCESS) {
        throw std::runtime_error("ChunkRenderer: Failed to allocate Shadow SSBO descriptor sets!");
    }

    std::vector<VkDescriptorSetLayout> cullLayouts(MAX_FRAMES_IN_FLIGHT, m_cullDescriptorSetLayout);
    allocInfo.pSetLayouts = cullLayouts.data();
    if (vkAllocateDescriptorSets(m_context.getDevice(), &allocInfo, m_cameraCullDescriptorSets) != VK_SUCCESS) {
        throw std::runtime_error("ChunkRenderer: Failed to allocate Camera cull descriptor sets!");
    }
    if (vkAllocateDescriptorSets(m_context.getDevice(), &allocInfo, m_shadowCullDescriptorSets) != VK_SUCCESS) {
        throw std::runtime_error("ChunkRenderer: Failed to allocate Shadow cull descriptor sets!");
    }
}

void ChunkRenderer::createComputePipeline() {
    const std::vector<char> shaderCode = readBinaryFile("bin/shaders/culling.comp.spv");
    VkShaderModule shaderModule = createShaderModule(m_context.getDevice(), shaderCode);

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(CullPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_cullDescriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    if (vkCreatePipelineLayout(m_context.getDevice(), &layoutInfo, nullptr, &m_cullPipelineLayout) != VK_SUCCESS) {
        vkDestroyShaderModule(m_context.getDevice(), shaderModule, nullptr);
        throw std::runtime_error("ChunkRenderer: Failed to create cull pipeline layout!");
    }

    VkPipelineShaderStageCreateInfo shaderStage{};
    shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStage.module = shaderModule;
    shaderStage.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = shaderStage;
    pipelineInfo.layout = m_cullPipelineLayout;

    if (vkCreateComputePipelines(m_context.getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_cullPipeline) != VK_SUCCESS) {
        vkDestroyShaderModule(m_context.getDevice(), shaderModule, nullptr);
        throw std::runtime_error("ChunkRenderer: Failed to create cull compute pipeline!");
    }

    vkDestroyShaderModule(m_context.getDevice(), shaderModule, nullptr);
}

void ChunkRenderer::createBuffers() {
    VkDeviceSize instanceBufferSize = MAX_VISIBLE_CHUNKS * sizeof(ChunkInstanceData);
    VkDeviceSize indirectBufferSize = MAX_VISIBLE_CHUNKS * sizeof(VkDrawIndexedIndirectCommand);
    constexpr VmaAllocationCreateFlags hostWriteFlags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        m_instanceBuffers[i] = std::make_unique<gfx::Buffer>(
            m_context,
            instanceBufferSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU,
            hostWriteFlags
        );

        m_cameraIndirectBuffers[i] = std::make_unique<gfx::Buffer>(
            m_context,
            indirectBufferSize,
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU,
            hostWriteFlags
        );

        m_shadowIndirectBuffers[i] = std::make_unique<gfx::Buffer>(
            m_context,
            indirectBufferSize,
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU,
            hostWriteFlags
        );
        VkDescriptorBufferInfo instanceInfo{};
        instanceInfo.buffer = m_instanceBuffers[i]->getBuffer();
        instanceInfo.offset = 0;
        instanceInfo.range = instanceBufferSize;

        VkDescriptorBufferInfo cameraIndirectInfo{};
        cameraIndirectInfo.buffer = m_cameraIndirectBuffers[i]->getBuffer();
        cameraIndirectInfo.offset = 0;
        cameraIndirectInfo.range = indirectBufferSize;

        VkDescriptorBufferInfo shadowIndirectInfo{};
        shadowIndirectInfo.buffer = m_shadowIndirectBuffers[i]->getBuffer();
        shadowIndirectInfo.offset = 0;
        shadowIndirectInfo.range = indirectBufferSize;

        VkWriteDescriptorSet writes[6]{};
        // Camera Set
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_cameraDescriptorSets[i];
        writes[0].dstBinding = 0;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &instanceInfo;

        // Shadow Set reuses the same instance buffer layout.
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_shadowDescriptorSets[i];
        writes[1].dstBinding = 0;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &instanceInfo;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = m_cameraCullDescriptorSets[i];
        writes[2].dstBinding = 0;
        writes[2].dstArrayElement = 0;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].descriptorCount = 1;
        writes[2].pBufferInfo = &instanceInfo;

        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = m_cameraCullDescriptorSets[i];
        writes[3].dstBinding = 1;
        writes[3].dstArrayElement = 0;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[3].descriptorCount = 1;
        writes[3].pBufferInfo = &cameraIndirectInfo;

        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = m_shadowCullDescriptorSets[i];
        writes[4].dstBinding = 0;
        writes[4].dstArrayElement = 0;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[4].descriptorCount = 1;
        writes[4].pBufferInfo = &instanceInfo;

        writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[5].dstSet = m_shadowCullDescriptorSets[i];
        writes[5].dstBinding = 1;
        writes[5].dstArrayElement = 0;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[5].descriptorCount = 1;
        writes[5].pBufferInfo = &shadowIndirectInfo;

        vkUpdateDescriptorSets(m_context.getDevice(), 6, writes, 0, nullptr);
    }
    std::cout << "[ChunkRenderer] MDI and SSBO Buffers initialized. Max visible chunks: " << MAX_VISIBLE_CHUNKS << "\n";

    // Persistent SSBO: Reserve CPU-side buffers
    m_cpuInstanceData.reserve(MAX_VISIBLE_CHUNKS);
    m_fadeStartTimes.reserve(MAX_VISIBLE_CHUNKS);
    m_activeBatches.reserve(128); // avoid heap allocation during hot rendering loop
}

void ChunkRenderer::clear() {
    m_meshWorker.waitAll();
    m_meshWorker.collect();
    m_renderData.clear();
    m_dirtyPending.clear();
    m_cpuInstanceData.clear();
    m_fadeStartTimes.clear();
    m_renderSnapshot.clear();
    m_renderSnapshotIndices.clear();
    m_sortedChunks.clear();
    m_activeBatches.clear();
    m_listDirty = true;
    m_framesDirty = {true, true, true};
    m_activeInstances = 0;
    m_totalVertices = 0;
    m_totalIndices = 0;
    m_visibleCount = 0;
    m_culledCount = 0;
    m_visibleVertices = 0;
}

void ChunkRenderer::upsertRenderSnapshot(const IVec3Key& key, const ChunkRenderData& rd, int lod) {
    if (!rd.mesh || !rd.valid) {
        eraseRenderSnapshot(key);
        return;
    }

    RenderChunkSnapshot snapshot{};
    snapshot.key = key;
    snapshot.poolIndex = rd.mesh->getBufferIndex();
    snapshot.indexCount = rd.indexCount;
    snapshot.firstIndex = rd.mesh->getFirstIndex();
    snapshot.vertexOffset = rd.mesh->getVertexOffset();
    snapshot.lod = lod;
    snapshot.fadeStartTime = rd.fadeStartTime;
    snapshot.fadeProgress = rd.fadeProgress;

    auto indexIt = m_renderSnapshotIndices.find(key);
    if (indexIt == m_renderSnapshotIndices.end()) {
        const size_t newIndex = m_renderSnapshot.size();
        m_renderSnapshot.push_back(snapshot);
        m_renderSnapshotIndices.emplace(key, newIndex);
    } else {
        m_renderSnapshot[indexIt->second] = snapshot;
    }
}

void ChunkRenderer::eraseRenderSnapshot(const IVec3Key& key) {
    auto indexIt = m_renderSnapshotIndices.find(key);
    if (indexIt == m_renderSnapshotIndices.end()) {
        return;
    }

    const size_t removeIndex = indexIt->second;
    const size_t lastIndex = m_renderSnapshot.size() - 1;
    if (removeIndex != lastIndex) {
        const RenderChunkSnapshot moved = m_renderSnapshot[lastIndex];
        m_renderSnapshot[removeIndex] = moved;
        m_renderSnapshotIndices[moved.key] = removeIndex;
    }

    m_renderSnapshot.pop_back();
    m_renderSnapshotIndices.erase(indexIt);
}

bool ChunkRenderer::isSnapshotVisibleInFrustum(const RenderChunkSnapshot& snapshot, const scene::Frustum& frustum) const {
    const auto aabb = buildAABB(snapshot.key.x, snapshot.key.y, snapshot.key.z);
    return frustum.isVisible(aabb);
}



scene::AABB ChunkRenderer::buildAABB(int cx, int cy, int cz) const {
    float wx = static_cast<float>(cx * CHUNK_SIZE);
    float wy = static_cast<float>(cy * CHUNK_SIZE);
    float wz = static_cast<float>(cz * CHUNK_SIZE);
    float sz = static_cast<float>(CHUNK_SIZE);
    return {{wx, wy, wz}, {wx + sz, wy + sz, wz + sz}};
}

void ChunkRenderer::markDirty(int cx, int cy, int cz) {
    auto* chunk = m_storage.getChunk(cx, cy, cz);
    if (!chunk) return;
    // Guard: only mesh chunks whose voxel data is fully ready.
    // Submitting UNGENERATED or GENERATING chunks yields empty meshes.
    if (chunk->m_state.load(std::memory_order_acquire) != ChunkState::READY) return;
    // Skip known-empty chunks (all-air / fully-occluded).
    // These are re-enabled by forceMarkDirty() when voxel data actually changes.
    auto rdIt = m_renderData.find({cx, cy, cz});
    if (rdIt != m_renderData.end() && rdIt->second.isEmpty) return;
    m_dirtyPending.insert({cx, cy, cz});
}

void ChunkRenderer::forceMarkDirty(int cx, int cy, int cz) {
    auto* chunk = m_storage.getChunk(cx, cy, cz);
    if (!chunk) return;
    if (chunk->m_state.load(std::memory_order_acquire) != ChunkState::READY) return;
    // Clear the isEmpty flag so the chunk gets re-meshed after a voxel edit.
    auto rdIt = m_renderData.find({cx, cy, cz});
    if (rdIt != m_renderData.end()) rdIt->second.isEmpty = false;
    m_dirtyPending.insert({cx, cy, cz});
}

void ChunkRenderer::clearEmptyFlag(int cx, int cy, int cz) {
    auto rdIt = m_renderData.find({cx, cy, cz});
    if (rdIt != m_renderData.end()) rdIt->second.isEmpty = false;
}

void ChunkRenderer::flushDirty() {
    if (m_dirtyPending.empty()) return;

    std::vector<MeshTask> batch;
    batch.reserve(m_dirtyPending.size());

    // 1) Evaluate Frustum, LODs, & push visible
    for (const auto& key : m_dirtyPending) {
        auto chunk = m_storage.getChunk(key.x, key.y, key.z);
        if (!chunk) continue;
        chunk->markDirty();

        int lod = chunk->m_currentLOD.load(std::memory_order_relaxed);
        if (lod < 0) lod = m_lodCtrl.calculateLOD(key.x, key.y, key.z); // fallback if unassigned

        std::array<const Chunk*, 6> neighbors = {
            m_storage.getChunk(key.x + 1, key.y, key.z),
            m_storage.getChunk(key.x - 1, key.y, key.z),
            m_storage.getChunk(key.x, key.y + 1, key.z),
            m_storage.getChunk(key.x, key.y - 1, key.z),
            m_storage.getChunk(key.x, key.y, key.z + 1),
            m_storage.getChunk(key.x, key.y, key.z - 1)
        };
        
        std::array<int, 6> nLODs = {
            m_lodCtrl.calculateLOD(key.x + 1, key.y, key.z),
            m_lodCtrl.calculateLOD(key.x - 1, key.y, key.z),
            m_lodCtrl.calculateLOD(key.x, key.y + 1, key.z),
            m_lodCtrl.calculateLOD(key.x, key.y - 1, key.z),
            m_lodCtrl.calculateLOD(key.x, key.y, key.z + 1),
            m_lodCtrl.calculateLOD(key.x, key.y, key.z - 1)
        };

        MeshTask task;
        task.chunk = chunk;
        task.neighbors = neighbors;
        task.neighborLODs = nLODs;
        task.cx = key.x;
        task.cy = key.y;
        task.cz = key.z;
        task.lod = lod;
        batch.push_back(std::move(task));
    }
    m_dirtyPending.clear();

    if (!batch.empty()) {
        m_meshWorker.submitBatchHigh(batch);
    }
}

void ChunkRenderer::submitGenerateTaskHigh(Chunk* chunk, const TerrainConfig& config) {
    if (!chunk) return;
    MeshTask task;
    task.type = MeshTask::Type::GENERATE;
    task.chunk = chunk;
    task.cx = chunk->getCX();
    task.cy = chunk->getCY();
    task.cz = chunk->getCZ();
    task.config = config;
    
    std::vector<MeshTask> batch;
    batch.push_back(std::move(task));
    m_meshWorker.submitBatchHigh(batch);
}

void ChunkRenderer::submitGenerateTaskLow(Chunk* chunk, const TerrainConfig& config) {
    if (!chunk) return;
    MeshTask task;
    task.type = MeshTask::Type::GENERATE;
    task.chunk = chunk;
    task.cx = chunk->getCX();
    task.cy = chunk->getCY();
    task.cz = chunk->getCZ();
    task.config = config;

    std::vector<MeshTask> batch;
    batch.push_back(std::move(task));
    m_meshWorker.submitBatchLow(batch);  // LOW priority: won't starve mesh rebuilds
}

void ChunkRenderer::waitAllWorkers() {
    m_meshWorker.waitAll();
}

std::array<uint32_t, 3> ChunkRenderer::getLODCounts() const {
    std::array<uint32_t, 3> out{0, 0, 0};
    for (const auto& snapshot : m_renderSnapshot) {
        int lod = snapshot.lod;
        if (lod >= 0 && lod <= 2) out[static_cast<size_t>(lod)]++;
    }
    return out;
}

bool ChunkRenderer::hasMesh() const {
    for (const auto& [key, rd] : m_renderData) {
        if (rd.valid && rd.mesh) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Persistent SSBO helpers
// ---------------------------------------------------------------------------

void ChunkRenderer::rebuildSortedList() {
    m_sortedChunks.clear();
    m_sortedChunks.reserve(m_renderSnapshot.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_renderSnapshot.size()); ++i) {
        const auto& snapshot = m_renderSnapshot[i];
        m_sortedChunks.push_back({i, snapshot.poolIndex, snapshot.lod});
    }
    std::sort(m_sortedChunks.begin(), m_sortedChunks.end(), [](const ChunkDrawCmd& a, const ChunkDrawCmd& b) {
        if (a.poolIndex != b.poolIndex) return a.poolIndex < b.poolIndex;
        return a.lod < b.lod;
    });
}

void ChunkRenderer::rebuildCpuInstanceData() {
    m_cpuInstanceData.clear();
    m_fadeStartTimes.clear();
    m_cpuInstanceData.reserve(m_sortedChunks.size());
    m_fadeStartTimes.reserve(m_sortedChunks.size());

    for (const auto& cmd : m_sortedChunks) {
        const auto& snapshot = m_renderSnapshot[cmd.snapshotIndex];
        ChunkInstanceData inst;
        inst.posX         = static_cast<float>(snapshot.key.x * CHUNK_SIZE);
        inst.posY         = static_cast<float>(snapshot.key.y * CHUNK_SIZE);
        inst.posZ         = static_cast<float>(snapshot.key.z * CHUNK_SIZE);
        inst.fadeProgress = snapshot.fadeProgress;
        m_cpuInstanceData.push_back(inst);
        m_fadeStartTimes.push_back(snapshot.fadeStartTime);
    }

    // Всі GPU буфери застаріли — потрібен memcpy для кожного кадру
    m_framesDirty = {true, true, true};
}

void ChunkRenderer::rebuildIndirectBuffers(uint32_t frame) {
    auto& cameraIndirects = m_cameraIndirectCpu[frame];

    m_activeBatches.clear();
    if (m_sortedChunks.empty()) return;

    // Pre-reserve to avoid heap allocation during the hot loop
    m_activeBatches.reserve(m_sortedChunks.size());

    uint32_t currentPool = m_sortedChunks[0].poolIndex;
    uint32_t startIdx = 0;

    for (uint32_t idx = 0; idx < static_cast<uint32_t>(m_sortedChunks.size()); ++idx) {
        const auto& cmd = m_sortedChunks[idx];
        const auto& snapshot = m_renderSnapshot[cmd.snapshotIndex];
        if (cmd.poolIndex != currentPool) {
            m_activeBatches.push_back({currentPool, startIdx, idx - startIdx});
            currentPool = cmd.poolIndex;
            startIdx = idx;
        }

        cameraIndirects[idx].indexCount    = snapshot.indexCount;
        cameraIndirects[idx].instanceCount = 0;
        cameraIndirects[idx].firstIndex    = snapshot.firstIndex;
        cameraIndirects[idx].vertexOffset  = snapshot.vertexOffset;
        cameraIndirects[idx].firstInstance = idx;

    }
    m_activeBatches.push_back({currentPool, startIdx,
        static_cast<uint32_t>(m_sortedChunks.size()) - startIdx});

    const size_t uploadSize = m_sortedChunks.size() * sizeof(VkDrawIndexedIndirectCommand);
    m_cameraIndirectBuffers[frame]->upload(cameraIndirects.data(), uploadSize);
}

void ChunkRenderer::cull(VkCommandBuffer cmd, const scene::Frustum& cameraFrustum, const scene::Frustum& shadowFrustum, const core::math::Vec3& cameraPos, float shadowDistanceLimit, float currentTime, uint32_t currentFrame) {
    (void)cmd;
    // 1. Якщо список чанків змінився (load/unload) — перебудуємо sorted list і CPU-буфер.
    // m_listDirty встановлюється ТІЛЬКИ у rebuildDirtyChunks / removeChunk / unloadMeshOnly.
    // Після rebuild знімаємо m_listDirty, щоб наступний кадр НЕ перебудовував даремно.
    if (m_listDirty) {
        rebuildSortedList();
        rebuildCpuInstanceData(); // встановлює m_framesDirty = {true, true, true}
        m_listDirty = false;      // структурна зміна оброблена — більше не будемо
    }

    // 2. Оновити fadeProgress на боці CPU (лише незавершені fade — O(нових чанків))
    bool fadeUpdated = false;
    for (size_t i = 0; i < m_cpuInstanceData.size(); i++) {
        float& fp = m_cpuInstanceData[i].fadeProgress;
        if (fp < 1.0f) {
            fp = std::clamp((currentTime - m_fadeStartTimes[i]) / 1.0f, 0.0f, 1.0f);
            fadeUpdated = true;
        }
    }
    if (fadeUpdated) m_framesDirty[currentFrame] = true;

    // 2.5 Renderer-side visibility stats come from the compact render snapshot,
    // not from indirect-buffer readback.
    uint32_t visibleCount = 0;
    uint32_t visibleIndexCount = 0;
    for (const auto& snapshot : m_renderSnapshot) {
        if (!isSnapshotVisibleInFrustum(snapshot, cameraFrustum)) {
            continue;
        }
        ++visibleCount;
        visibleIndexCount += snapshot.indexCount;
    }
    m_visibleCount = visibleCount;
    m_visibleVertices = visibleIndexCount / 3;

    // 3. Один memcpy якщо GPU-буфер поточного кадру застарів
    if (m_framesDirty[currentFrame] && !m_cpuInstanceData.empty()) {
        size_t sz = m_cpuInstanceData.size() * sizeof(ChunkInstanceData);
        m_instanceBuffers[currentFrame]->upload(m_cpuInstanceData.data(), sz);
        rebuildIndirectBuffers(currentFrame);
        m_framesDirty[currentFrame] = false;
    }

    m_activeInstances = static_cast<uint32_t>(m_cpuInstanceData.size());
    m_culledCount = m_activeInstances > visibleCount ? (m_activeInstances - visibleCount) : 0;

    if (m_activeInstances == 0) {
        return;
    }

    runCpuCullFallback(cameraFrustum, shadowFrustum, cameraPos, shadowDistanceLimit, currentFrame);
}

void ChunkRenderer::runCpuCullFallback(const scene::Frustum& cameraFrustum, const scene::Frustum& shadowFrustum, const core::math::Vec3& cameraPos, float shadowDistanceLimit, uint32_t currentFrame) {
    (void)shadowFrustum;
    (void)cameraPos;
    (void)shadowDistanceLimit;

    // 4. CPU-side frustum filtering updates the CPU-side indirect command staging arrays.
    auto& cameraIndirects = m_cameraIndirectCpu[currentFrame];

    for (uint32_t idx = 0; idx < static_cast<uint32_t>(m_sortedChunks.size()); ++idx) {
        const auto& drawCmd = m_sortedChunks[idx];
        const auto& snapshot = m_renderSnapshot[drawCmd.snapshotIndex];

        const bool cameraVisible = isSnapshotVisibleInFrustum(snapshot, cameraFrustum);
        cameraIndirects[idx].instanceCount = cameraVisible ? 1u : 0u;
    }

    const size_t uploadSize = m_sortedChunks.size() * sizeof(VkDrawIndexedIndirectCommand);
    m_cameraIndirectBuffers[currentFrame]->upload(cameraIndirects.data(), uploadSize);
}

void ChunkRenderer::renderCamera(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t currentFrame) {
    if (m_activeInstances == 0) return;

    // Both camera and shadow passes need only Set 2 which contains the SSBO buffers
    // Camera Pass uses cameraDescriptorSets, which links to cameraIndirectBuffers (if needed) and instanceBuffers
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        layout, 2, 1, &m_cameraDescriptorSets[currentFrame], 0, nullptr);

    VkBuffer indirectBuffer = m_cameraIndirectBuffers[currentFrame]->getBuffer();
    uint32_t stride = sizeof(VkDrawIndexedIndirectCommand);

    for (const auto& batch : m_activeBatches) {
        m_geometryManager.bindPool(cmd, batch.poolIndex);
        vkCmdDrawIndexedIndirect(cmd, indirectBuffer, batch.startIdx * stride, batch.count, stride);
    }
    // m_visibleCount is populated from the renderer-owned CPU snapshot visibility pass in cull().
}

void ChunkRenderer::renderShadow(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t currentFrame) {
    if (m_activeInstances == 0) return;

    // Shadow pass uses shadowDescriptorSets and shadowIndirectBuffers
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        layout, 2, 1, &m_shadowDescriptorSets[currentFrame], 0, nullptr);

    VkBuffer indirectBuffer = m_shadowIndirectBuffers[currentFrame]->getBuffer();
    uint32_t stride = sizeof(VkDrawIndexedIndirectCommand);

    for (const auto& batch : m_activeBatches) {
        m_geometryManager.bindPool(cmd, batch.poolIndex);
        vkCmdDrawIndexedIndirect(cmd, indirectBuffer, batch.startIdx * stride, batch.count, stride);
    }
    // We don't update m_visibleCount here, since camera pass represents the main frame stats
}

void ChunkRenderer::rebuildDirtyChunks(VkDevice device, float currentTime) {
    auto t0 = std::chrono::high_resolution_clock::now();

    auto done = m_meshWorker.collect();
    if (done.empty()) return;

    // Deduplicate: keep only the most-recently-completed task per chunk.
    // This prevents uploading an outdated LOD result when the worker queue
    // delivered multiple results for the same chunk in one collect() batch.
    std::unordered_map<IVec3Key, MeshTask, IVec3Hash> latestTasks;
    latestTasks.reserve(done.size());
    for (auto& task : done) {
        IVec3Key key{task.cx, task.cy, task.cz};
        auto chunk = m_storage.getChunk(key.x, key.y, key.z);

        if (task.type == MeshTask::Type::GENERATE) {
            if (!chunk) {
                continue;
            }
            latestTasks[key] = std::move(task);
            continue;
        }

        if (!chunk) {
            continue;
        }

        int desiredLOD = chunk->m_currentLOD.load(std::memory_order_relaxed);
        if (desiredLOD < 0) {
            continue;
        }
        if (task.lod == desiredLOD) {
            latestTasks[key] = std::move(task);
        }
        // else: stale LOD result — silently discard
    }

    if (!latestTasks.empty() && device != VK_NULL_HANDLE) {
        // vkDeviceWaitIdle(device); // ВИДАЛЕНО: Pipeline Stall виправлено через Delayed Free у GeometryManager!
    }

    std::vector<gfx::GeometryManager::UploadRequest> requests;
    requests.reserve(latestTasks.size());

    for (auto& [key, task] : latestTasks) {
        auto chunk = m_storage.getChunk(key.x, key.y, key.z);
        if (!chunk) {
            continue;
        }

        if (task.type != MeshTask::Type::GENERATE) {
            int desiredLOD = chunk->m_currentLOD.load(std::memory_order_relaxed);
            if (desiredLOD < 0 || task.lod != desiredLOD) {
                continue;
            }
        }

        auto& rd = m_renderData[key];

        if (rd.valid) {
            m_totalVertices -= rd.vertexCount;
            m_totalIndices  -= rd.indexCount;
            if (rd.mesh) {
                int32_t vOff = rd.mesh->getVertexOffset();
                uint32_t iOff = rd.mesh->getFirstIndex();
                m_geometryManager.freeMesh(vOff, iOff, 
                    rd.vertexCount * sizeof(VoxelVertex), rd.indexCount * sizeof(uint32_t), sizeof(VoxelVertex), rd.mesh->getBufferIndex());
            }
            rd.mesh.reset();
            rd.valid = false;
            eraseRenderSnapshot(key);
            m_listDirty = true;
            m_framesDirty = {true, true, true};
        }

        if (task.type == MeshTask::Type::GENERATE) {
            // Voxels are ready — queue this chunk for meshing on the next flushDirty().
            // Do NOT submit here: we need neighbour data which may also be freshly generated.
            m_dirtyPending.insert(IVec3Key{key.x, key.y, key.z});
            continue;
        }

        if (task.result.empty()) {
            // Chunk is all-air or fully occluded — no geometry needed.
            // Tag it so markDirty() silently ignores future LOD-cascade notifications.
            rd.isEmpty = true;
            // Always clear the dirty flag regardless of LOD level (Bug fix: previously
            // markClean was only called for lod==0, leaving LOD1/2 chunks permanently dirty).
            if (chunk) chunk->markClean();
            // No SSBO change needed — empty chunks don't occupy a slot.
            continue;
        }

        // Non-empty result: clear any stale isEmpty flag (chunk gained geometry).
        rd.isEmpty = false;

        // Voxel vertices are already generated in local chunk space [0, CHUNK_SIZE]!
        // No worldBias shifting needed. The position is sent per-chunk via PushConstants.

        gfx::GeometryManager::UploadRequest req;
        rd.mesh.reset(m_geometryManager.allocateMeshRaw(static_cast<uint32_t>(task.result.vertices.size()), static_cast<uint32_t>(task.result.indices.size()), req, task.result.vertices, task.result.indices));
        rd.aabb = buildAABB(key.x, key.y, key.z);
        rd.vertexCount = static_cast<uint32_t>(task.result.vertices.size());
        rd.indexCount  = static_cast<uint32_t>(task.result.indices.size());
        rd.valid = true;
        rd.fadeStartTime = currentTime;
        rd.fadeProgress  = 0.0f; // новий mesh — fade з 0
        upsertRenderSnapshot(key, rd, task.lod);
        m_listDirty = true;             // список змінився — потрібен rebuild
        m_framesDirty = {true, true, true};

        m_totalVertices += rd.vertexCount;
        m_totalIndices  += rd.indexCount;

        requests.push_back(req);

        if (chunk) chunk->markClean();
    }

    if (!requests.empty()) {
        m_geometryManager.executeBatchUpload(requests);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    m_lastRebuildMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
}


void ChunkRenderer::removeChunk(const IVec3Key& key) {
    auto it = m_renderData.find(key);
    if (it != m_renderData.end()) {
        ChunkRenderData& rd = it->second;
        if (rd.mesh) {
            int32_t  vOff = rd.mesh->getVertexOffset();
            uint32_t iOff = rd.mesh->getFirstIndex();
            m_geometryManager.freeMesh(vOff, iOff, 
                rd.vertexCount * sizeof(VoxelVertex), rd.indexCount * sizeof(uint32_t), sizeof(VoxelVertex), rd.mesh->getBufferIndex());
            m_totalVertices -= rd.vertexCount;
            m_totalIndices  -= rd.indexCount;
        }
        eraseRenderSnapshot(key);
        m_renderData.erase(it);
        m_listDirty = true;
        m_framesDirty = {true, true, true};
    }
    m_dirtyPending.erase(key);
}

void ChunkRenderer::unloadMeshOnly(const IVec3Key& key) {
    // Tier-3: звільняємо GPU пам'ять, але залишаємо LOD_EVICTED у m_chunkLOD.
    auto it = m_renderData.find(key);
    if (it != m_renderData.end()) {
        ChunkRenderData& rd = it->second;
        if (rd.mesh) {
            int32_t  vOff = rd.mesh->getVertexOffset();
            uint32_t iOff = rd.mesh->getFirstIndex();
            m_geometryManager.freeMesh(vOff, iOff,
                rd.vertexCount * sizeof(VoxelVertex), rd.indexCount * sizeof(uint32_t), sizeof(VoxelVertex), rd.mesh->getBufferIndex());
            m_totalVertices -= rd.vertexCount;
            m_totalIndices  -= rd.indexCount;
        }
        eraseRenderSnapshot(key);
        m_renderData.erase(it);
        m_listDirty = true;
        m_framesDirty = {true, true, true};
    }
    // Ключова відмінність від removeChunk: ставимо sentinel LOD_EVICTED,
    // а не erase — щоб updateCamera знала "цей чанк вивантажено свідомо".
    Chunk* chunk = m_storage.getChunk(key.x, key.y, key.z);
    if (chunk) chunk->m_currentLOD.store(LOD_EVICTED, std::memory_order_relaxed);
    m_dirtyPending.erase(key);
}



} // namespace world
