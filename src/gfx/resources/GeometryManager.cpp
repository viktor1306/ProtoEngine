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
    std::cout << "Global Geometry Buffers allocated." << std::endl;
}

Mesh* GeometryManager::uploadMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    VkDeviceSize vertexDataSize = vertices.size() * sizeof(Vertex);
    VkDeviceSize indexDataSize  = indices.size()  * sizeof(uint32_t);

    if (m_vertexOffset + vertexDataSize > VERTEX_BUFFER_SIZE) throw std::runtime_error("Global Vertex Buffer full!");
    if (m_indexOffset  + indexDataSize  > INDEX_BUFFER_SIZE)  throw std::runtime_error("Global Index Buffer full!");

    Buffer vertexStaging(m_context, vertexDataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    vertexStaging.upload(vertices.data(), vertexDataSize);
    Buffer indexStaging(m_context, indexDataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    indexStaging.upload(indices.data(), indexDataSize);

    VkCommandBuffer cmd = m_context.beginSingleTimeCommands();

    VkBufferCopy vc{0, m_vertexOffset, vertexDataSize};
    vkCmdCopyBuffer(cmd, vertexStaging.getBuffer(), m_globalVertexBuffer->getBuffer(), 1, &vc);
    VkBufferCopy ic{0, m_indexOffset, indexDataSize};
    vkCmdCopyBuffer(cmd, indexStaging.getBuffer(), m_globalIndexBuffer->getBuffer(), 1, &ic);

    VkBufferMemoryBarrier2 vb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    vb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; vb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    vb.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT; vb.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
    vb.srcQueueFamilyIndex = vb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vb.buffer = m_globalVertexBuffer->getBuffer(); vb.offset = m_vertexOffset; vb.size = vertexDataSize;

    VkBufferMemoryBarrier2 ib{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    ib.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; ib.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    ib.dstStageMask = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT; ib.dstAccessMask = VK_ACCESS_2_INDEX_READ_BIT;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.buffer = m_globalIndexBuffer->getBuffer(); ib.offset = m_indexOffset; ib.size = indexDataSize;

    VkBufferMemoryBarrier2 barriers[] = {vb, ib};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 2; dep.pBufferMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(cmd, &dep);

    m_context.endSingleTimeCommands(cmd);

    uint32_t firstIndex   = static_cast<uint32_t>(m_indexOffset  / sizeof(uint32_t));
    int32_t  vertexOffset = static_cast<int32_t> (m_vertexOffset / sizeof(Vertex));

    m_vertexOffset += vertexDataSize;
    m_indexOffset  += indexDataSize;

    return new Mesh(static_cast<uint32_t>(indices.size()), firstIndex, vertexOffset);
}

void GeometryManager::bind(VkCommandBuffer commandBuffer) {
    VkBuffer     vbufs[]   = {m_globalVertexBuffer->getBuffer()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vbufs, offsets);
    vkCmdBindIndexBuffer(commandBuffer, m_globalIndexBuffer->getBuffer(), 0, VK_INDEX_TYPE_UINT32);
}

} // namespace gfx
