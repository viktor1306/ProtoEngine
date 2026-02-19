#include "GeometryManager.hpp"
#include <stdexcept>
#include <iostream>

namespace gfx {

GeometryManager::~GeometryManager() = default;

GeometryManager::GeometryManager(VulkanContext& context) : m_context(context) {
    m_globalVertexBuffer = std::make_unique<Buffer>(context, m_totalVertexCapacity,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);
    m_globalIndexBuffer = std::make_unique<Buffer>(context, m_totalIndexCapacity,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    m_vertexAllocator.reset(m_totalVertexCapacity);
    m_indexAllocator.reset(m_totalIndexCapacity);

    std::cout << "[GeometryManager] Allocated: "
              << (m_totalVertexCapacity / 1024 / 1024) << " MB vertex + "
              << (m_totalIndexCapacity  / 1024 / 1024) << " MB index buffers with FreeList allocator.\n";
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

    std::vector<VkBufferCopy> vCopies;
    std::vector<VkBufferCopy> iCopies;
    vCopies.reserve(requests.size());
    iCopies.reserve(requests.size());

    VkDeviceSize vStagingOffset = 0;
    VkDeviceSize iStagingOffset = 0;

    for (const auto& req : requests) {
        if (req.vertexBytes > 0) {
            std::memcpy(vMapped + vStagingOffset, req.vertexData, req.vertexBytes);
            vCopies.push_back({vStagingOffset, req.vertexOffset, req.vertexBytes});
            vStagingOffset += req.vertexBytes;
        }
        if (req.indexBytes > 0) {
            std::memcpy(iMapped + iStagingOffset, req.indexData, req.indexBytes);
            iCopies.push_back({iStagingOffset, req.indexOffset, req.indexBytes});
            iStagingOffset += req.indexBytes;
        }
    }

    if (totalVertexBytes > 0) vertexStaging.unmap();
    if (totalIndexBytes > 0) indexStaging.unmap();

    VkCommandBuffer cmd = m_context.beginSingleTimeCommands();

    if (!vCopies.empty()) {
        vkCmdCopyBuffer(cmd, vertexStaging.getBuffer(), m_globalVertexBuffer->getBuffer(), static_cast<uint32_t>(vCopies.size()), vCopies.data());
    }
    if (!iCopies.empty()) {
        vkCmdCopyBuffer(cmd, indexStaging.getBuffer(), m_globalIndexBuffer->getBuffer(), static_cast<uint32_t>(iCopies.size()), iCopies.data());
    }

    std::vector<VkBufferMemoryBarrier2> barriers;
    if (!vCopies.empty()) {
        VkBufferMemoryBarrier2 vb = {};
        vb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        vb.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        vb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        vb.dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        vb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        vb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vb.buffer = m_globalVertexBuffer->getBuffer();
        vb.offset = 0;
        vb.size   = VK_WHOLE_SIZE;
        barriers.push_back(vb);
    }
    if (!iCopies.empty()) {
        VkBufferMemoryBarrier2 ib = {};
        ib.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        ib.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
        ib.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        ib.dstStageMask  = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        ib.dstAccessMask = VK_ACCESS_2_INDEX_READ_BIT;
        ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.buffer = m_globalIndexBuffer->getBuffer();
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
    m_vertexAllocator.reset(m_totalVertexCapacity);
    m_indexAllocator.reset(m_totalIndexCapacity);
}

// ---------------------------------------------------------------------------
// bind — bind global vertex + index buffers for any vertex type
// ---------------------------------------------------------------------------
void GeometryManager::bind(VkCommandBuffer commandBuffer) {
    VkBuffer     vbufs[]   = {m_globalVertexBuffer->getBuffer()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vbufs, offsets);
    vkCmdBindIndexBuffer(commandBuffer, m_globalIndexBuffer->getBuffer(), 0, VK_INDEX_TYPE_UINT32);
}

} // namespace gfx
