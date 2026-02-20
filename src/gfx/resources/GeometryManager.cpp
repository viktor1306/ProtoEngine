#include "GeometryManager.hpp"
#include <stdexcept>
#include <iostream>
#include <unordered_set>

namespace gfx {

GeometryManager::~GeometryManager() = default;

GeometryManager::GeometryManager(VulkanContext& context) : m_context(context) {
    allocateNewPool();
}

uint32_t GeometryManager::allocateNewPool() {
    auto pool = std::make_unique<BufferPool>();
    
    pool->vertexBuffer = std::make_unique<Buffer>(m_context, m_totalVertexCapacity,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);
    pool->indexBuffer = std::make_unique<Buffer>(m_context, m_totalIndexCapacity,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    pool->vertexAllocator.reset(m_totalVertexCapacity);
    pool->indexAllocator.reset(m_totalIndexCapacity);

    m_pools.push_back(std::move(pool));
    
    std::cout << "[GeometryManager] Allocated new buffer pool [" << (m_pools.size() - 1) << "]: "
              << (m_totalVertexCapacity / 1024 / 1024) << " MB vertex + "
              << (m_totalIndexCapacity  / 1024 / 1024) << " MB index buffers.\n";
              
    return static_cast<uint32_t>(m_pools.size() - 1);
}

// ---------------------------------------------------------------------------
// executeBatchUpload — create one big staging buffer and issue 1 barrier for all
// ---------------------------------------------------------------------------
void GeometryManager::executeBatchUpload(const std::vector<UploadRequest>& requests) {
    if (requests.empty()) return;

    VkDeviceSize totalVertexBytes = 0;
    VkDeviceSize totalIndexBytes  = 0;
    for (const auto& req : requests) {
        totalVertexBytes += req.vertexBytes;
        totalIndexBytes  += req.indexBytes;
    }

    if (totalVertexBytes == 0 && totalIndexBytes == 0) return;

    Buffer vertexStaging(m_context, std::max<VkDeviceSize>(1, totalVertexBytes), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    Buffer indexStaging(m_context, std::max<VkDeviceSize>(1, totalIndexBytes), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

    uint8_t* vMapped = nullptr;
    if (totalVertexBytes > 0) vertexStaging.map((void**)&vMapped);
    uint8_t* iMapped = nullptr;
    if (totalIndexBytes > 0) indexStaging.map((void**)&iMapped);

    VkDeviceSize vStagingOffset = 0;
    VkDeviceSize iStagingOffset = 0;

    VkCommandBuffer cmd = m_context.beginSingleTimeCommands();
    
    std::unordered_set<uint32_t> dirtyPools;

    for (const auto& req : requests) {
        dirtyPools.insert(req.bufferIndex);
        if (req.vertexBytes > 0) {
            std::memcpy(vMapped + vStagingOffset, req.vertexData, req.vertexBytes);
            VkBufferCopy copy = {vStagingOffset, req.vertexOffset, req.vertexBytes};
            vkCmdCopyBuffer(cmd, vertexStaging.getBuffer(), m_pools[req.bufferIndex]->vertexBuffer->getBuffer(), 1, &copy);
            vStagingOffset += req.vertexBytes;
        }
        if (req.indexBytes > 0) {
            std::memcpy(iMapped + iStagingOffset, req.indexData, req.indexBytes);
            VkBufferCopy copy = {iStagingOffset, req.indexOffset, req.indexBytes};
            vkCmdCopyBuffer(cmd, indexStaging.getBuffer(), m_pools[req.bufferIndex]->indexBuffer->getBuffer(), 1, &copy);
            iStagingOffset += req.indexBytes;
        }
    }

    if (totalVertexBytes > 0) vertexStaging.unmap();
    if (totalIndexBytes > 0) indexStaging.unmap();

    std::vector<VkBufferMemoryBarrier2> barriers;
    for (uint32_t poolIdx : dirtyPools) {
        VkBufferMemoryBarrier2 vb = {};
        vb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        vb.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        vb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        vb.dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        vb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        vb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vb.buffer = m_pools[poolIdx]->vertexBuffer->getBuffer();
        vb.offset = 0;
        vb.size   = VK_WHOLE_SIZE;
        barriers.push_back(vb);

        VkBufferMemoryBarrier2 ib = {};
        ib.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        ib.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        ib.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        ib.dstStageMask  = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        ib.dstAccessMask = VK_ACCESS_2_INDEX_READ_BIT;
        ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.buffer = m_pools[poolIdx]->indexBuffer->getBuffer();
        ib.offset = 0;
        ib.size   = VK_WHOLE_SIZE;
        barriers.push_back(ib);
    }

    if (!barriers.empty()) {
        VkDependencyInfo dep = {};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.bufferMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dep.pBufferMemoryBarriers    = barriers.data();
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    m_context.endSingleTimeCommands(cmd);
}

// ---------------------------------------------------------------------------
// uploadMesh — standard gfx::Vertex path (existing pipeline)
// ---------------------------------------------------------------------------
Mesh* GeometryManager::uploadMesh(const std::vector<Vertex>& vertices,
                                  const std::vector<uint32_t>& indices)
{
    if (vertices.empty() || indices.empty()) return nullptr;
    UploadRequest req;
    Mesh* m = allocateMeshRaw(static_cast<uint32_t>(vertices.size()), static_cast<uint32_t>(indices.size()), req, vertices, indices);
    executeBatchUpload({req});
    return m;
}

// ---------------------------------------------------------------------------
// reset — rewind offsets (GPU must be idle before calling)
// ---------------------------------------------------------------------------
void GeometryManager::reset() {
    std::lock_guard<std::mutex> lock(m_poolMutex);
    for (auto& pool : m_pools) {
        pool->vertexAllocator.reset(m_totalVertexCapacity);
        pool->indexAllocator.reset(m_totalIndexCapacity);
    }
}

// ---------------------------------------------------------------------------
// bindPool — bind specific vertex + index buffers for any vertex type
// ---------------------------------------------------------------------------
void GeometryManager::bindPool(VkCommandBuffer commandBuffer, uint32_t poolIndex) {
    if (poolIndex >= m_pools.size()) return;
    VkBuffer     vbufs[]   = {m_pools[poolIndex]->vertexBuffer->getBuffer()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vbufs, offsets);
    vkCmdBindIndexBuffer(commandBuffer, m_pools[poolIndex]->indexBuffer->getBuffer(), 0, VK_INDEX_TYPE_UINT32);
}

VkDeviceSize GeometryManager::getVertexBytesUsed() const {
    VkDeviceSize total = 0;
    for (const auto& pool : m_pools) total += pool->vertexAllocator.m_allocated_bytes;
    return total;
}

VkDeviceSize GeometryManager::getIndexBytesUsed() const {
    VkDeviceSize total = 0;
    for (const auto& pool : m_pools) total += pool->indexAllocator.m_allocated_bytes;
    return total;
}

} // namespace gfx
