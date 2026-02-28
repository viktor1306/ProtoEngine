#include "ChunkRenderer.hpp"
#include <chrono>
#include <iostream>
#include <algorithm>
#include <fstream>

namespace world {

ChunkRenderer::ChunkRenderer(gfx::VulkanContext& context, gfx::GeometryManager& geom, ChunkStorage& storage, LODController& lodCtrl, uint32_t meshWorkerThreads)
    : m_context(context), m_geometryManager(geom), m_storage(storage), m_lodCtrl(lodCtrl), m_meshWorker(meshWorkerThreads)
{
    std::cout << "[ChunkRenderer] MeshWorker threads: "
              << m_meshWorker.getThreadCount() << "\n" << std::flush;
              
    createDescriptorSetLayout();
    createBuffers();
    createComputePipeline();
    
    std::cout << "[ChunkRenderer] MDI and SSBO Buffers initialized. Max visible chunks: " << MAX_VISIBLE_CHUNKS << "\n";
    std::cout << "[ChunkRenderer] DescriptorSetLayout = " << m_descriptorSetLayout << "\n";
}

ChunkRenderer::~ChunkRenderer() {
    VkDevice device = m_context.getDevice();
    if (m_computePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, m_computePipeline, nullptr);
    if (m_computePipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, m_computePipelineLayout, nullptr);
    
    vkDestroyDescriptorPool(device, m_descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(device, m_descriptorSetLayout, nullptr);
}

void ChunkRenderer::createDescriptorSetLayout() {
    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
    };

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(m_context.getDevice(), &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("ChunkRenderer: Failed to create SSBO descriptor set layout!");
    }

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT;
    
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;

    if (vkCreateDescriptorPool(m_context.getDevice(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("ChunkRenderer: Failed to create descriptor pool!");
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

void ChunkRenderer::createBuffers() {
    VkDeviceSize instanceBufferSize = MAX_VISIBLE_CHUNKS * sizeof(ChunkInstanceData);
    VkDeviceSize indirectBufferSize = MAX_VISIBLE_CHUNKS * sizeof(VkDrawIndexedIndirectCommand);

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
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

        VkDescriptorBufferInfo instanceInfo{};
        instanceInfo.buffer = m_instanceBuffers[i]->getBuffer();
        instanceInfo.offset = 0;
        instanceInfo.range = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo indirectInfo{};
        indirectInfo.buffer = m_indirectBuffers[i]->getBuffer();
        indirectInfo.offset = 0;
        indirectInfo.range = VK_WHOLE_SIZE;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = m_descriptorSets[i];
        writes[0].dstBinding = 0;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &instanceInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = m_descriptorSets[i];
        writes[1].dstBinding = 1;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &indirectInfo;

        vkUpdateDescriptorSets(m_context.getDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

void ChunkRenderer::createComputePipeline() {
    VkDevice device = m_context.getDevice();
    
    auto readFile = [](const std::string& filename) {
        std::ifstream file(filename, std::ios::ate | std::ios::binary);
        if (!file.is_open()) throw std::runtime_error("failed to open " + filename);
        size_t fileSize = (size_t)file.tellg();
        std::vector<char> buffer(fileSize);
        file.seekg(0);
        file.read(buffer.data(), fileSize);
        file.close();
        return buffer;
    };
    
    auto code = readFile("bin/shaders/culling.comp.spv");
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
    
    VkShaderModule computeShaderModule;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &computeShaderModule) != VK_SUCCESS) {
        throw std::runtime_error("failed to create compute shader module!");
    }
    
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset = 0;
    pcRange.size = sizeof(float)*4*6 + sizeof(uint32_t)*4; // 6 planes (vec4) + align
    
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pcRange;
    
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &m_computePipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("failed to create compute pipeline layout!");
    }
    
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = computeShaderModule;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = m_computePipelineLayout;
    
    if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_computePipeline) != VK_SUCCESS) {
        throw std::runtime_error("failed to create compute pipeline!");
    }
    
    vkDestroyShaderModule(device, computeShaderModule, nullptr);
}

void ChunkRenderer::clear() {
    m_meshWorker.waitAll();
    m_meshWorker.collect();
    m_renderData.clear();
    m_chunkLOD.clear();
    m_dirtyPending.clear();
    m_totalVertices = 0;
    m_totalIndices = 0;
    m_visibleCount = 0;
    m_culledCount = 0;
    m_visibleVertices = 0;
    
    m_listDirty = true;
    for(int i=0; i<MAX_FRAMES_IN_FLIGHT; ++i) m_gpuBuffersDirty[i] = true;
}

scene::AABB ChunkRenderer::buildAABB(int cx, int cy, int cz) const {
    float wx = static_cast<float>(cx * CHUNK_SIZE);
    float wy = static_cast<float>(cy * CHUNK_SIZE);
    float wz = static_cast<float>(cz * CHUNK_SIZE);
    float sz = static_cast<float>(CHUNK_SIZE);
    return {{wx, wy, wz}, {wx + sz, wy + sz, wz + sz}};
}

void ChunkRenderer::markDirty(int cx, int cy, int cz) {
    if (!m_storage.getChunk(cx, cy, cz)) return;
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

        MeshTask task;
        task.chunk = chunk;
        task.neighbors = neighbors;
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
    m_meshWorker.submitBatchLow(batch);
}

void ChunkRenderer::setLOD(const IVec3Key& key, int lod) {
    m_chunkLOD[key] = lod;
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
        vkDeviceWaitIdle(device);
    }

    std::vector<gfx::GeometryManager::UploadRequest> requests;
    requests.reserve(latestTasks.size());

    for (auto& [key, task] : latestTasks) {

        if (task.type != MeshTask::Type::GENERATE) {
            auto lodIt = m_chunkLOD.find(key);
            int desiredLOD = (lodIt != m_chunkLOD.end()) ? lodIt->second : 0;
            if (task.lod != desiredLOD) continue;
        }

        auto& rd = m_renderData[key];
        
        if (task.type == MeshTask::Type::GENERATE) {
            if (task.chunk) {   
                ChunkState expected = ChunkState::GENERATING;
                task.chunk->m_state.compare_exchange_strong(expected, ChunkState::READY, std::memory_order_acq_rel);
            }
            continue;
        }

        if (rd.mesh) {
            uint32_t vOff = rd.mesh->getVertexOffset();
            uint32_t iOff = rd.mesh->getFirstIndex();
            m_geometryManager.freeMesh(vOff, iOff,
                rd.vertexCount * sizeof(VoxelVertex), rd.indexCount * sizeof(uint32_t), sizeof(VoxelVertex), rd.mesh->getBufferIndex());
            m_totalVertices -= rd.vertexCount;
            m_totalIndices  -= rd.indexCount;
            m_listDirty = true;
        }

        if (task.result.vertices.empty() || task.result.indices.empty()) {
            rd.valid = false;
            auto it = m_chunkLOD.find(key);
            if(it != m_chunkLOD.end()) {
                m_listDirty = true;
            }
            continue;
        }

        rd.vertexCount = static_cast<uint32_t>(task.result.vertices.size());
        rd.indexCount  = static_cast<uint32_t>(task.result.indices.size());
        rd.aabb = buildAABB(key.x, key.y, key.z);
        rd.valid = true;
        
        if (rd.fadeStartTime == 0.0f) {
            rd.fadeStartTime = currentTime;
        }

        gfx::GeometryManager::UploadRequest req;
        rd.mesh.reset(m_geometryManager.allocateMeshRaw(rd.vertexCount, rd.indexCount, req, task.result.vertices, task.result.indices));
        requests.push_back(req);

        m_totalVertices += rd.vertexCount;
        m_totalIndices  += rd.indexCount;
        m_listDirty = true;
    }

    if (!requests.empty()) {
        m_geometryManager.executeBatchUpload(requests);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    m_lastRebuildMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
}

void ChunkRenderer::cull(VkCommandBuffer cmd, const scene::Frustum& frustum, float currentTime, uint32_t currentFrame) {
    auto* instances = static_cast<ChunkInstanceData*>(m_instanceMapped[currentFrame]);
    auto* indirects = static_cast<VkDrawIndexedIndirectCommand*>(m_indirectMapped[currentFrame]);

    fprintf(stderr, "[Cull] Starting cull...\n"); fflush(stderr);

    if (m_listDirty) {
        fprintf(stderr, "[Cull] m_listDirty is true. Clearing active lists...\n"); fflush(stderr);
        m_sortedChunks.clear();
        for (auto& [key, rd] : m_renderData) {
            if (!rd.valid || !rd.mesh) continue;
            m_sortedChunks.push_back({key, rd.mesh->getBufferIndex(), m_chunkLOD[key], &rd});
        }
        
        fprintf(stderr, "[Cull] Sorting %zu chunks...\n", m_sortedChunks.size()); fflush(stderr);
        std::sort(m_sortedChunks.begin(), m_sortedChunks.end(), [](const ChunkDrawCmd& a, const ChunkDrawCmd& b) {
            if (a.poolIndex != b.poolIndex) return a.poolIndex < b.poolIndex;
            return a.lod < b.lod; 
        });

        m_activeBatches.clear();
        m_activeInstances = 0;
        m_cpuInstances.clear();
        m_cpuIndirects.clear();

        fprintf(stderr, "[Cull] Building CPU instances...\n"); fflush(stderr);

        if (!m_sortedChunks.empty()) {
            m_activeBatches.reserve(m_sortedChunks.size());
            m_cpuInstances.reserve(m_sortedChunks.size());
            m_cpuIndirects.reserve(m_sortedChunks.size());

            uint32_t currentPool = m_sortedChunks[0].poolIndex;
            uint32_t startIdx = 0;

            for (const auto& cmdDraw : m_sortedChunks) {
                if (cmdDraw.poolIndex != currentPool) {
                    m_activeBatches.emplace_back(currentPool, startIdx, m_activeInstances - startIdx);
                    currentPool = cmdDraw.poolIndex;
                    startIdx = m_activeInstances;
                }

                auto* rd = cmdDraw.rd;
                uint32_t idx = m_activeInstances++;

                fprintf(stderr, "  [Loop] Iter %u, pool %u, rd: %p\n", idx, currentPool, (void*)rd); fflush(stderr);

                ChunkInstanceData inst{};
                inst.posX = static_cast<float>(cmdDraw.key.x * CHUNK_SIZE);
                inst.posY = static_cast<float>(cmdDraw.key.y * CHUNK_SIZE);
                inst.posZ = static_cast<float>(cmdDraw.key.z * CHUNK_SIZE);
                inst.fadeProgress = 1.0f; 
                
                fprintf(stderr, "  [Loop] Pushing cpuInstance...\n"); fflush(stderr);
                m_cpuInstances.push_back(inst);

                VkDrawIndexedIndirectCommand cmdInd{};
                cmdInd.indexCount    = rd->indexCount;
                cmdInd.instanceCount = 0; 
                
                fprintf(stderr, "  [Loop] Getting mesh offsets from rd->mesh...\n"); fflush(stderr);
                cmdInd.firstIndex    = rd->mesh->getFirstIndex();
                cmdInd.vertexOffset  = rd->mesh->getVertexOffset();
                cmdInd.firstInstance = idx; 
                
                fprintf(stderr, "  [Loop] Pushing cpuIndirect...\n"); fflush(stderr);
                m_cpuIndirects.push_back(cmdInd);
            }
            fprintf(stderr, "  [Loop] Exited loop. Pushing active batch...\n"); fflush(stderr);
            m_activeBatches.emplace_back(currentPool, startIdx, m_activeInstances - startIdx);
            fprintf(stderr, "  [Loop] Active batch pushed.\n"); fflush(stderr);
        }
        
        fprintf(stderr, "[Cull] Marking dirty for all frames...\n"); fflush(stderr);
        for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) m_gpuBuffersDirty[i] = true;
        m_listDirty = false;
        
        fprintf(stderr, "[Cull] m_listDirty completed.\n"); fflush(stderr);
    }

    if (m_activeInstances == 0) return;

    fprintf(stderr, "[Cull] instances ptr: %p, indirects ptr: %p\n", (void*)instances, (void*)indirects);
    fflush(stderr);

    if (m_gpuBuffersDirty[currentFrame]) {
        if (!m_cpuInstances.empty()) {
            memcpy(instances, m_cpuInstances.data(), m_activeInstances * sizeof(ChunkInstanceData));
            memcpy(indirects, m_cpuIndirects.data(), m_activeInstances * sizeof(VkDrawIndexedIndirectCommand));
        }
        m_gpuBuffersDirty[currentFrame] = false;
    }

    for (size_t i = 0; i < m_activeInstances; i++) {
        instances[i].fadeProgress = std::clamp((currentTime - m_sortedChunks[i].rd->fadeStartTime) / 1.0f, 0.0f, 1.0f);
    }

    fprintf(stderr, "[Cull] Binding pipeline...\n"); fflush(stderr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);

    fprintf(stderr, "[Cull] Binding descriptor sets...\n"); fflush(stderr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipelineLayout, 0, 1, &m_descriptorSets[currentFrame], 0, nullptr);

    struct ComputePushConstants {
        float planes[24];
        uint32_t count;
    } pc;

    const auto& frustumPlanes = frustum.getPlanes();
    for (int i = 0; i < 6; i++) {
        pc.planes[i * 4 + 0] = frustumPlanes[i].normal.x;
        pc.planes[i * 4 + 1] = frustumPlanes[i].normal.y;
        pc.planes[i * 4 + 2] = frustumPlanes[i].normal.z;
        pc.planes[i * 4 + 3] = frustumPlanes[i].d;
    }
    pc.count = m_activeInstances;

    fprintf(stderr, "[Cull] Pushing constants...\n"); fflush(stderr);
    vkCmdPushConstants(cmd, m_computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &pc);

    uint32_t groupCountX = (m_activeInstances + 63) / 64;
    fprintf(stderr, "[Cull] Dispatching compute (groups: %u)...\n", groupCountX); fflush(stderr);
    vkCmdDispatch(cmd, groupCountX, 1, 1);

    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;

    VkDependencyInfo depInfo{};
    depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    depInfo.memoryBarrierCount = 1;
    depInfo.pMemoryBarriers = &barrier;

    fprintf(stderr, "[Cull] Submitting barrier...\n"); fflush(stderr);
    vkCmdPipelineBarrier2(cmd, &depInfo);
    fprintf(stderr, "[Cull] Done.\n"); fflush(stderr);
}

void ChunkRenderer::render(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t currentFrame) {
    m_visibleCount    = m_activeInstances; 
    m_culledCount     = 0; 
    m_visibleVertices = 0; // GPU culling hides this info unless parsed back

    if (m_activeInstances == 0) return;

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, 1, &m_descriptorSets[currentFrame], 0, nullptr);

    VkBuffer indirectBuf = m_indirectBuffers[currentFrame]->getBuffer();

    for (const auto& batch : m_activeBatches) {
        if (batch.count == 0) continue;
        m_geometryManager.bindPool(cmd, batch.poolIndex);
        
        VkDeviceSize offset = batch.startIdx * sizeof(VkDrawIndexedIndirectCommand);
        vkCmdDrawIndexedIndirect(cmd, indirectBuf, offset, batch.count, sizeof(VkDrawIndexedIndirectCommand));
    }
}

void ChunkRenderer::waitAllWorkers() {
    m_meshWorker.waitAll();
}

void ChunkRenderer::removeChunk(const IVec3Key& key) {
    auto it = m_renderData.find(key);
    if (it != m_renderData.end()) {
        auto& rd = it->second;
        if (rd.mesh) {
            uint32_t vOff = rd.mesh->getVertexOffset();
            uint32_t iOff = rd.mesh->getFirstIndex();
            m_geometryManager.freeMesh(vOff, iOff,
                rd.vertexCount * sizeof(VoxelVertex), rd.indexCount * sizeof(uint32_t), sizeof(VoxelVertex), rd.mesh->getBufferIndex());
            m_totalVertices -= rd.vertexCount;
            m_totalIndices  -= rd.indexCount;
        }
        m_renderData.erase(it);
        m_listDirty = true;
    }
    m_chunkLOD.erase(key);
}

void ChunkRenderer::unloadMeshOnly(const IVec3Key& key) {
    auto it = m_renderData.find(key);
    if (it != m_renderData.end()) {
        auto& rd = it->second;
        if (rd.mesh) {
            uint32_t vOff = rd.mesh->getVertexOffset();
            uint32_t iOff = rd.mesh->getFirstIndex();
            m_geometryManager.freeMesh(vOff, iOff,
                rd.vertexCount * sizeof(VoxelVertex), rd.indexCount * sizeof(uint32_t), sizeof(VoxelVertex), rd.mesh->getBufferIndex());
            m_totalVertices -= rd.vertexCount;
            m_totalIndices  -= rd.indexCount;
        }
        rd.valid = false;
        rd.mesh.reset();
        m_listDirty = true;
    }
    m_chunkLOD[key] = LOD_EVICTED;
}

} // namespace world
