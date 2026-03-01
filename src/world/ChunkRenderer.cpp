#include "ChunkRenderer.hpp"
#include "gfx/rendering/Pipeline.hpp"
#include <chrono>
#include <iostream>
#include <fstream>

namespace world {

ChunkRenderer::ChunkRenderer(gfx::VulkanContext& context, gfx::GeometryManager& geom, ChunkStorage& storage, LODController& lodCtrl, uint32_t meshWorkerThreads)
    : m_context(context), m_geometryManager(geom), m_storage(storage), m_lodCtrl(lodCtrl), m_meshWorker(meshWorkerThreads)
{
    std::cout << "[ChunkRenderer] MeshWorker threads: "
              << m_meshWorker.getThreadCount() << "\n" << std::flush;
              
    createDescriptorSetLayout();
    createComputePipeline();
    createBuffers();
}

ChunkRenderer::~ChunkRenderer() {
    VkDevice device = m_context.getDevice();
    if (m_computePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, m_computePipeline, nullptr);
    if (m_computePipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, m_computePipelineLayout, nullptr);
    
    vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, m_descriptorSetLayout, nullptr);
    
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (m_instanceBuffers[i]) m_instanceBuffers[i]->unmap();
        if (m_cameraIndirectBuffers[i]) m_cameraIndirectBuffers[i]->unmap();
        if (m_shadowIndirectBuffers[i]) m_shadowIndirectBuffers[i]->unmap();
    }
}

void ChunkRenderer::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding bindings[2]{};
    
    // Binding 0: SSBO (InstanceData)
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    // Binding 1: SSBO (IndirectCmds)
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(m_context.getDevice(), &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("ChunkRenderer: Failed to create SSBO descriptor set layout!");
    }

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT * 2; // Для InstanceBuffer (Camera + Shadow)
    
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT * 2; // Для IndirectBuffer (Camera + Shadow)

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT * 2;

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
}

static std::vector<char> readShaderFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("failed to open " + filename);
    size_t fileSize = (size_t) file.tellg();
    std::vector<char> buffer(fileSize);
    file.seekg(0); file.read(buffer.data(), fileSize);
    return buffer;
}

void ChunkRenderer::createComputePipeline() {
    auto compCode = readShaderFile("bin/shaders/culling.comp.spv");
    
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = compCode.size();
    moduleInfo.pCode = reinterpret_cast<const uint32_t*>(compCode.data());
    
    VkShaderModule compModule;
    if (vkCreateShaderModule(m_context.getDevice(), &moduleInfo, nullptr, &compModule) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create compute shader module!");
    }
    
    VkPipelineShaderStageCreateInfo shaderStage{};
    shaderStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    shaderStage.module = compModule;
    shaderStage.pName = "main";
    
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = 128; // 6 planes + maxDist/pos + count aligned to 16 bytes (128 bytes total)
    
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    
    if (vkCreatePipelineLayout(m_context.getDevice(), &layoutInfo, nullptr, &m_computePipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create compute pipeline layout!");
    }
    
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = shaderStage;
    pipelineInfo.layout = m_computePipelineLayout;
    
    if (vkCreateComputePipelines(m_context.getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_computePipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create compute pipeline!");
    }
    
    vkDestroyShaderModule(m_context.getDevice(), compModule, nullptr);
}

void ChunkRenderer::createBuffers() {
    VkDeviceSize instanceBufferSize = MAX_VISIBLE_CHUNKS * sizeof(ChunkInstanceData);
    VkDeviceSize indirectBufferSize = MAX_VISIBLE_CHUNKS * sizeof(VkDrawIndexedIndirectCommand);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        m_instanceBuffers[i] = std::make_unique<gfx::Buffer>(
            m_context,
            instanceBufferSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU
        );
        m_instanceBuffers[i]->map(&m_instanceMapped[i]);

        m_cameraIndirectBuffers[i] = std::make_unique<gfx::Buffer>(
            m_context,
            indirectBufferSize,
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU
        );
        m_cameraIndirectBuffers[i]->map(&m_cameraIndirectMapped[i]);

        m_shadowIndirectBuffers[i] = std::make_unique<gfx::Buffer>(
            m_context,
            indirectBufferSize,
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU
        );
        m_shadowIndirectBuffers[i]->map(&m_shadowIndirectMapped[i]);

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

        VkWriteDescriptorSet writes[4]{};
        // Camera Set
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_cameraDescriptorSets[i];
        writes[0].dstBinding = 0;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &instanceInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_cameraDescriptorSets[i];
        writes[1].dstBinding = 1;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &cameraIndirectInfo;

        // Shadow Set (Uses SAME instance buffer, different indirect buffer)
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = m_shadowDescriptorSets[i];
        writes[2].dstBinding = 0;
        writes[2].dstArrayElement = 0;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].descriptorCount = 1;
        writes[2].pBufferInfo = &instanceInfo;

        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = m_shadowDescriptorSets[i];
        writes[3].dstBinding = 1;
        writes[3].dstArrayElement = 0;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[3].descriptorCount = 1;
        writes[3].pBufferInfo = &shadowIndirectInfo;

        vkUpdateDescriptorSets(m_context.getDevice(), 4, writes, 0, nullptr);
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
    m_sortedChunks.clear();
    m_listDirty = true;
    m_framesDirty = {true, true, true};
    m_totalVertices = 0;
    m_totalIndices = 0;
    m_visibleCount = 0;
    m_culledCount = 0;
    m_visibleVertices = 0;
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
    for (const auto& ac : m_storage.getChunks()) {
        if (!ac.chunk) continue;
        int lod = ac.chunk->m_currentLOD.load(std::memory_order_relaxed);
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
    for (const auto& [key, rd] : m_renderData) {
        if (!rd.valid || !rd.mesh) continue;
        Chunk* chunk = m_storage.getChunk(key.x, key.y, key.z);
        if (!chunk) continue;
        int lod = chunk->m_currentLOD.load(std::memory_order_relaxed);
        m_sortedChunks.push_back({key, rd.mesh->getBufferIndex(), lod});
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
        const auto& rd = m_renderData[cmd.key];
        ChunkInstanceData inst;
        inst.posX         = static_cast<float>(cmd.key.x * CHUNK_SIZE);
        inst.posY         = static_cast<float>(cmd.key.y * CHUNK_SIZE);
        inst.posZ         = static_cast<float>(cmd.key.z * CHUNK_SIZE);
        inst.fadeProgress = rd.fadeProgress; // збережено з попереднього стану
        m_cpuInstanceData.push_back(inst);
        m_fadeStartTimes.push_back(rd.fadeStartTime);
    }

    // Всі GPU буфери застаріли — потрібен memcpy для кожного кадру
    m_framesDirty = {true, true, true};
}

void ChunkRenderer::rebuildIndirectBuffers(uint32_t frame) {
    auto* cameraIndirects = static_cast<VkDrawIndexedIndirectCommand*>(m_cameraIndirectMapped[frame]);
    auto* shadowIndirects = static_cast<VkDrawIndexedIndirectCommand*>(m_shadowIndirectMapped[frame]);

    m_activeBatches.clear();
    if (m_sortedChunks.empty()) return;

    // Pre-reserve to avoid heap allocation during the hot loop
    m_activeBatches.reserve(m_sortedChunks.size());

    uint32_t currentPool = m_sortedChunks[0].poolIndex;
    uint32_t startIdx = 0;

    for (uint32_t idx = 0; idx < static_cast<uint32_t>(m_sortedChunks.size()); ++idx) {
        const auto& cmd = m_sortedChunks[idx];
        if (cmd.poolIndex != currentPool) {
            m_activeBatches.push_back({currentPool, startIdx, idx - startIdx});
            currentPool = cmd.poolIndex;
            startIdx = idx;
        }

        const auto it = m_renderData.find(cmd.key);
        if (it == m_renderData.end() || !it->second.mesh) continue;
        const auto& rd = it->second;

        cameraIndirects[idx].indexCount    = rd.indexCount;
        cameraIndirects[idx].instanceCount = 0; // GPU Compute will set to 1 if visible
        cameraIndirects[idx].firstIndex    = rd.mesh->getFirstIndex();
        cameraIndirects[idx].vertexOffset  = rd.mesh->getVertexOffset();
        cameraIndirects[idx].firstInstance = idx;

        shadowIndirects[idx].indexCount    = rd.indexCount;
        shadowIndirects[idx].instanceCount = 0;
        shadowIndirects[idx].firstIndex    = rd.mesh->getFirstIndex();
        shadowIndirects[idx].vertexOffset  = rd.mesh->getVertexOffset();
        shadowIndirects[idx].firstInstance = idx;
    }
    m_activeBatches.push_back({currentPool, startIdx,
        static_cast<uint32_t>(m_sortedChunks.size()) - startIdx});

    // Save how many draw commands this frame slot contains so cull() can
    // read back GPU-written instanceCount values on the NEXT use of this slot.
    m_lastDispatchCount[frame] = static_cast<uint32_t>(m_sortedChunks.size());
}

void ChunkRenderer::cull(VkCommandBuffer cmd, const scene::Frustum& cameraFrustum, const scene::Frustum& shadowFrustum, const core::math::Vec3& cameraPos, float shadowDistanceLimit, float currentTime, uint32_t currentFrame) {

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

    // 2.5 Non-blocking async GPU readback — count GPU-culled visible chunks.
    // -----------------------------------------------------------------------
    // The fence for `currentFrame` was waited on inside Renderer::beginFrame().
    // That guarantees the GPU has FINISHED writing instanceCount (0 or 1) into
    // m_cameraIndirectMapped[currentFrame] during the *previous* use of this
    // slot (3 frames ago with MAX_FRAMES_IN_FLIGHT=3).  We read it BEFORE
    // rebuildIndirectBuffers() resets every instanceCount back to 0.
    // Memory note: VMA_MEMORY_USAGE_CPU_TO_GPU resolves to HOST_COHERENT on
    // virtually all desktop Vulkan drivers — no vkInvalidate needed.
    {
        const uint32_t prevCount = m_lastDispatchCount[currentFrame];
        if (prevCount > 0 && m_cameraIndirectMapped[currentFrame]) {
            const auto* cmds = static_cast<const VkDrawIndexedIndirectCommand*>(
                m_cameraIndirectMapped[currentFrame]);
            uint32_t visChunks  = 0;
            uint32_t visIndices = 0;
            for (uint32_t i = 0; i < prevCount; ++i) {
                if (cmds[i].instanceCount > 0) {
                    ++visChunks;
                    visIndices += cmds[i].indexCount;
                }
            }
            m_visibleCount    = visChunks;
            m_visibleVertices = visIndices / 3; // index count → triangle count
            m_culledCount     = prevCount - visChunks;
        }
    }

    // 3. Один memcpy якщо GPU-буфер поточного кадру застарів
    if (m_framesDirty[currentFrame] && !m_cpuInstanceData.empty()) {
        size_t sz = m_cpuInstanceData.size() * sizeof(ChunkInstanceData);
        memcpy(m_instanceMapped[currentFrame], m_cpuInstanceData.data(), sz);
        rebuildIndirectBuffers(currentFrame);
        m_framesDirty[currentFrame] = false;
    }

    m_activeInstances = static_cast<uint32_t>(m_cpuInstanceData.size());

    if (m_activeInstances == 0) return;

    // 4. GPU Compute Shader Dispatch
    struct ComputePush {
        core::math::Vec4 planes[6];
        core::math::Vec4 cameraPosAndLimit;
        uint32_t count;
    } push{};

    uint32_t groupCount = (m_activeInstances + 63) / 64;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);

    // --- DISPATCH 1: CAMERA ---
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipelineLayout, 0, 1, &m_cameraDescriptorSets[currentFrame], 0, nullptr);
    for (int i = 0; i < 6; i++) {
        const auto& p = cameraFrustum.getPlanes()[i];
        push.planes[i] = core::math::Vec4{p.normal.x, p.normal.y, p.normal.z, p.d};
    }
    push.cameraPosAndLimit = core::math::Vec4{cameraPos.x, cameraPos.y, cameraPos.z, 0.0f};
    push.count = m_activeInstances;
    vkCmdPushConstants(cmd, m_computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePush), &push);
    vkCmdDispatch(cmd, groupCount, 1, 1);

    // --- DISPATCH 2: SHADOW ---
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipelineLayout, 0, 1, &m_shadowDescriptorSets[currentFrame], 0, nullptr);
    for (int i = 0; i < 6; i++) {
        const auto& p = shadowFrustum.getPlanes()[i];
        push.planes[i] = core::math::Vec4{p.normal.x, p.normal.y, p.normal.z, p.d};
    }
    push.cameraPosAndLimit = core::math::Vec4{cameraPos.x, cameraPos.y, cameraPos.z, shadowDistanceLimit};
    push.count = m_activeInstances;
    vkCmdPushConstants(cmd, m_computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePush), &push);
    vkCmdDispatch(cmd, groupCount, 1, 1);

    // 5. Memory Barrier (Compute Write -> Graphic Indirect Read)
    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(cmd, &dep);
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
    // m_visibleCount is now populated via non-blocking GPU readback in cull().
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

        if (task.type == MeshTask::Type::GENERATE) {
            latestTasks[key] = std::move(task);
            continue;
        }

        auto chunk = m_storage.getChunk(key.x, key.y, key.z);
        int desiredLOD = chunk ? chunk->m_currentLOD.load(std::memory_order_relaxed) : 0;
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

        if (task.type != MeshTask::Type::GENERATE) {
            auto chunk = m_storage.getChunk(key.x, key.y, key.z);
            int desiredLOD = chunk ? chunk->m_currentLOD.load(std::memory_order_relaxed) : 0;
            if (task.lod != desiredLOD) {
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
            auto chunk = m_storage.getChunk(key.x, key.y, key.z);
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
        m_listDirty = true;             // список змінився — потрібен rebuild
        m_framesDirty = {true, true, true};

        m_totalVertices += rd.vertexCount;
        m_totalIndices  += rd.indexCount;

        requests.push_back(req);

        auto chunk = m_storage.getChunk(key.x, key.y, key.z);
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
