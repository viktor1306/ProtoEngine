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
        if (m_indirectBuffers[i]) m_indirectBuffers[i]->unmap();
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
    poolSizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT; // Для InstanceBuffer
    
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT; // Для IndirectBuffer

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;

    if (vkCreateDescriptorPool(m_context.getDevice(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("ChunkRenderer: Failed to create SSBO descriptor pool!");
    }

    std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, m_descriptorSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocInfo.pSetLayouts = layouts.data();

    if (vkAllocateDescriptorSets(m_context.getDevice(), &allocInfo, m_descriptorSets) != VK_SUCCESS) {
        throw std::runtime_error("ChunkRenderer: Failed to allocate SSBO descriptor sets!");
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
    pushRange.size = sizeof(float) * 24 + sizeof(uint32_t); // 6 planes * 4 floats + 1 uint = 100 bytes
    
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

        m_indirectBuffers[i] = std::make_unique<gfx::Buffer>(
            m_context,
            indirectBufferSize,
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU
        );
        m_indirectBuffers[i]->map(&m_indirectMapped[i]);

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_instanceBuffers[i]->getBuffer();
        bufferInfo.offset = 0;
        bufferInfo.range = instanceBufferSize;

        VkDescriptorBufferInfo indirectInfo{};
        indirectInfo.buffer = m_indirectBuffers[i]->getBuffer();
        indirectInfo.offset = 0;
        indirectInfo.range = indirectBufferSize;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_descriptorSets[i];
        writes[0].dstBinding = 0;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &bufferInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_descriptorSets[i];
        writes[1].dstBinding = 1;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &indirectInfo;

        vkUpdateDescriptorSets(m_context.getDevice(), 2, writes, 0, nullptr);
    }
    std::cout << "[ChunkRenderer] MDI and SSBO Buffers initialized. Max visible chunks: " << MAX_VISIBLE_CHUNKS << "\n";
}

void ChunkRenderer::clear() {
    m_meshWorker.waitAll();
    m_meshWorker.collect();
    m_renderData.clear();
    m_chunkLOD.clear();
    m_dirtyPending.clear();
    m_listDirty = true;
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
    m_dirtyPending.insert({cx, cy, cz});
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

        int lod = m_lodCtrl.calculateLOD(key.x, key.y, key.z);
        m_chunkLOD[key] = lod;

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

void ChunkRenderer::setLOD(const IVec3Key& key, int lod) {
    auto it = m_chunkLOD.find(key);
    if (it == m_chunkLOD.end() || it->second != lod) {
        m_chunkLOD[key] = lod;
        m_listDirty = true;
    }
}

int ChunkRenderer::getLOD(const IVec3Key& key) const {
    auto it = m_chunkLOD.find(key);
    return (it != m_chunkLOD.end()) ? it->second : -1;
}

std::array<uint32_t, 3> ChunkRenderer::getLODCounts() const {
    std::array<uint32_t, 3> out{0, 0, 0};
    for (const auto& [key, lod] : m_chunkLOD) {
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

void ChunkRenderer::cull(VkCommandBuffer cmd, const scene::Frustum& frustum, float currentTime, uint32_t currentFrame) {
    auto* instances = static_cast<ChunkInstanceData*>(m_instanceMapped[currentFrame]);
    auto* indirects = static_cast<VkDrawIndexedIndirectCommand*>(m_indirectMapped[currentFrame]);

    // 1. Sort chunks CPU-side for initial Early-Z (LOD 0 -> 2)
    if (m_listDirty) {
        m_sortedChunks.clear();
        for (const auto& [key, rd] : m_renderData) {
            if (!rd.valid || !rd.mesh) continue;
            m_sortedChunks.push_back({key, rd.mesh->getBufferIndex(), m_chunkLOD[key]});
        }
        
        std::sort(m_sortedChunks.begin(), m_sortedChunks.end(), [](const ChunkDrawCmd& a, const ChunkDrawCmd& b) {
            if (a.poolIndex != b.poolIndex) return a.poolIndex < b.poolIndex;
            return a.lod < b.lod; 
        });
        
        m_listDirty = false;
    }

    m_activeBatches.clear();
    m_activeInstances = 0;

    if (m_sortedChunks.empty()) return;

    // 2. Prepare dynamic buffers linearly
    uint32_t currentPool = m_sortedChunks[0].poolIndex;
    uint32_t startIdx = 0;

    for (const auto& cmdDraw : m_sortedChunks) {
        if (cmdDraw.poolIndex != currentPool) {
            m_activeBatches.push_back({currentPool, startIdx, m_activeInstances - startIdx});
            currentPool = cmdDraw.poolIndex;
            startIdx = m_activeInstances;
        }

        const auto& rd = m_renderData[cmdDraw.key];
        uint32_t idx = m_activeInstances++;

        // Fill SSBO Data
        instances[idx].posX = static_cast<float>(cmdDraw.key.x * CHUNK_SIZE);
        instances[idx].posY = static_cast<float>(cmdDraw.key.y * CHUNK_SIZE);
        instances[idx].posZ = static_cast<float>(cmdDraw.key.z * CHUNK_SIZE);
        instances[idx].fadeProgress = std::clamp((currentTime - rd.fadeStartTime) / 1.0f, 0.0f, 1.0f);

        // Fill non-culled Indirect command
        indirects[idx].indexCount    = rd.indexCount;
        indirects[idx].instanceCount = 0; // GPU Culling Compute Shader will set to 1 if visible
        indirects[idx].firstIndex    = rd.mesh->getFirstIndex();
        indirects[idx].vertexOffset  = rd.mesh->getVertexOffset();
        indirects[idx].firstInstance = idx; 
    }
    m_activeBatches.push_back({currentPool, startIdx, m_activeInstances - startIdx});

    // 3. GPU Compute Shader Dispatch
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipelineLayout, 0, 1, &m_descriptorSets[currentFrame], 0, nullptr);

    struct ComputePush {
        core::math::Vec4 planes[6];
        uint32_t count;
    } push{};
    
    for (int i = 0; i < 6; i++) {
        const auto& p = frustum.getPlanes()[i];
        push.planes[i] = core::math::Vec4{p.normal.x, p.normal.y, p.normal.z, p.d};
    }
    push.count = m_activeInstances;

    vkCmdPushConstants(cmd, m_computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePush), &push);

    uint32_t groupCount = (m_activeInstances + 63) / 64;
    vkCmdDispatch(cmd, groupCount, 1, 1);

    // 4. Memory Barrier (Compute Write -> Graphic Indirect Read)
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

void ChunkRenderer::render(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t currentFrame) {
    if (m_activeInstances == 0) return;

    // Bind SSBO Descriptor Set
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        layout, 2, 1, &m_descriptorSets[currentFrame], 0, nullptr);

    VkBuffer indirectBuffer = m_indirectBuffers[currentFrame]->getBuffer();
    uint32_t stride = sizeof(VkDrawIndexedIndirectCommand);

    for (const auto& batch : m_activeBatches) {
        m_geometryManager.bindPool(cmd, batch.poolIndex);
        vkCmdDrawIndexedIndirect(cmd, indirectBuffer, batch.startIdx * stride, batch.count, stride);
    }

    // Since GPU does the culling, we loosely set visibleCount
    m_visibleCount = m_activeInstances;
}

void ChunkRenderer::rebuildDirtyChunks(VkDevice device, float currentTime) {
    auto t0 = std::chrono::high_resolution_clock::now();

    auto done = m_meshWorker.collect();
    if (done.empty()) return;

    std::unordered_map<IVec3Key, MeshTask, IVec3Hash> latestTasks;
    for (auto& task : done) {
        IVec3Key key{task.cx, task.cy, task.cz};

        if (task.type == MeshTask::Type::GENERATE) {
            latestTasks[key] = std::move(task);
            continue;
        }

        auto lodIt = m_chunkLOD.find(key);
        int desiredLOD = (lodIt != m_chunkLOD.end()) ? lodIt->second : 0;
        if (task.lod == desiredLOD) {
            latestTasks[key] = std::move(task);
        }
    }

    if (!latestTasks.empty() && device != VK_NULL_HANDLE) {
        // vkDeviceWaitIdle(device); // ВИДАЛЕНО: Pipeline Stall виправлено через Delayed Free у GeometryManager!
    }

    std::vector<gfx::GeometryManager::UploadRequest> requests;
    requests.reserve(latestTasks.size());

    for (auto& [key, task] : latestTasks) {

        if (task.type != MeshTask::Type::GENERATE) {
            auto lodIt = m_chunkLOD.find(key);
            int desiredLOD = (lodIt != m_chunkLOD.end()) ? lodIt->second : 0;
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
        }

        if (task.type == MeshTask::Type::GENERATE) {
            // Task already generated voxels, now we just need to queue it for meshing.
            m_dirtyPending.insert(IVec3Key{key.x, key.y, key.z});
            continue; // Do not build mesh right away, it needs neighbor data
        }

        if (task.result.empty()) {
            if (task.lod == 0) {
                auto chunk = m_storage.getChunk(key.x, key.y, key.z);
                if (chunk) chunk->markClean();
            }
            continue;
        }

        // Voxel vertices are already generated in local chunk space [0, CHUNK_SIZE]!
        // No worldBias shifting needed. The position is sent per-chunk via PushConstants.

        gfx::GeometryManager::UploadRequest req;
        rd.mesh.reset(m_geometryManager.allocateMeshRaw(static_cast<uint32_t>(task.result.vertices.size()), static_cast<uint32_t>(task.result.indices.size()), req, task.result.vertices, task.result.indices));
        rd.aabb = buildAABB(key.x, key.y, key.z);
        rd.vertexCount = static_cast<uint32_t>(task.result.vertices.size());
        rd.indexCount  = static_cast<uint32_t>(task.result.indices.size());
        rd.valid = true;
        m_listDirty = true;

        rd.fadeStartTime = currentTime;

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
    }
    m_chunkLOD.erase(key);
    m_dirtyPending.erase(key);
    m_listDirty = true;
}

void ChunkRenderer::unloadMeshOnly(const IVec3Key& key) {
    // Tier-3: звільняємо GPU пам'ять, але залишаємо LOD_EVICTED у m_chunkLOD.
    // Це блокує LOD-логіку від повторного markDirty, поки чанк не повернеться
    // у зону видимості (Zone 2 або Tier-1/2) та не отримає реальний LOD.
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
    }
    // Ключова відмінність від removeChunk: ставимо sentinel LOD_EVICTED,
    // а не erase — щоб updateCamera знала "цей чанк вивантажено свідомо".
    m_chunkLOD[key] = LOD_EVICTED;
    m_dirtyPending.erase(key);
}



} // namespace world
