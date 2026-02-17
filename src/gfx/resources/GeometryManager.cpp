#include "GeometryManager.hpp"
#include <stdexcept>
#include <iostream>

namespace gfx {

GeometryManager::~GeometryManager() = default;

GeometryManager::GeometryManager(VulkanContext& context) : m_context(context) {
    m_globalVertexBuffer = std::make_unique<Buffer>(context, VERTEX_BUFFER_SIZE,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);
    m_globalIndexBuffer = std::make_unique<Buffer>(context, INDEX_BUFFER_SIZE,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);
    std::cout << "[GeometryManager] Allocated: "
              << (VERTEX_BUFFER_SIZE / 1024 / 1024) << " MB vertex + "
              << (INDEX_BUFFER_SIZE  / 1024 / 1024) << " MB index buffers.\n";
}

// ---------------------------------------------------------------------------
// uploadRawData — internal workhorse: stages and copies raw bytes to GPU
// ---------------------------------------------------------------------------
void GeometryManager::uploadRawData(const void*  vertexData, VkDeviceSize vertexBytes,
                                    const void*  indexData,  VkDeviceSize indexBytes)
{
    // Staging buffers (CPU → GPU)
    Buffer vertexStaging(m_context, vertexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    vertexStaging.upload(vertexData, vertexBytes);

    Buffer indexStaging(m_context, indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    indexStaging.upload(indexData, indexBytes);

    VkCommandBuffer cmd = m_context.beginSingleTimeCommands();

    // Copy vertex data
    VkBufferCopy vc{0, m_vertexOffset, vertexBytes};
    vkCmdCopyBuffer(cmd, vertexStaging.getBuffer(), m_globalVertexBuffer->getBuffer(), 1, &vc);

    // Copy index data
    VkBufferCopy ic{0, m_indexOffset, indexBytes};
    vkCmdCopyBuffer(cmd, indexStaging.getBuffer(), m_globalIndexBuffer->getBuffer(), 1, &ic);

    // Sync 2 barriers — ensure copies are visible to vertex/index input stages
    VkBufferMemoryBarrier2 vb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    vb.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
    vb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    vb.dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    vb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    vb.srcQueueFamilyIndex = vb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vb.buffer = m_globalVertexBuffer->getBuffer();
    vb.offset = m_vertexOffset;
    vb.size   = vertexBytes;

    VkBufferMemoryBarrier2 ib{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    ib.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
    ib.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    ib.dstStageMask  = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
    ib.dstAccessMask = VK_ACCESS_2_INDEX_READ_BIT;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.buffer = m_globalIndexBuffer->getBuffer();
    ib.offset = m_indexOffset;
    ib.size   = indexBytes;

    VkBufferMemoryBarrier2 barriers[] = {vb, ib};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 2;
    dep.pBufferMemoryBarriers    = barriers;
    vkCmdPipelineBarrier2(cmd, &dep);

    m_context.endSingleTimeCommands(cmd);
}

// ---------------------------------------------------------------------------
// uploadMesh — standard gfx::Vertex path (existing pipeline)
// ---------------------------------------------------------------------------
Mesh* GeometryManager::uploadMesh(const std::vector<Vertex>& vertices,
                                  const std::vector<uint32_t>& indices)
{
    VkDeviceSize vertexDataSize = vertices.size() * sizeof(Vertex);
    VkDeviceSize indexDataSize  = indices.size()  * sizeof(uint32_t);

    if (m_vertexOffset + vertexDataSize > VERTEX_BUFFER_SIZE)
        throw std::runtime_error("GeometryManager: Vertex buffer full!");
    if (m_indexOffset  + indexDataSize  > INDEX_BUFFER_SIZE)
        throw std::runtime_error("GeometryManager: Index buffer full!");

    uploadRawData(vertices.data(), vertexDataSize, indices.data(), indexDataSize);

    uint32_t firstIndex   = static_cast<uint32_t>(m_indexOffset  / sizeof(uint32_t));
    int32_t  vertexOffset = static_cast<int32_t> (m_vertexOffset / sizeof(Vertex));

    m_vertexOffset += vertexDataSize;
    m_indexOffset  += indexDataSize;

    return new Mesh(static_cast<uint32_t>(indices.size()), firstIndex, vertexOffset);
}

// ---------------------------------------------------------------------------
// reset — rewind offsets (GPU must be idle before calling)
// ---------------------------------------------------------------------------
void GeometryManager::reset() {
    m_vertexOffset = 0;
    m_indexOffset  = 0;
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
