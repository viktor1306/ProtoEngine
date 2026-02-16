#include "GeometryManager.hpp"
#include <cstring>
#include <stdexcept>
#include <iostream>

namespace gfx {

GeometryManager::GeometryManager(VulkanContext& context) : m_context(context) {
    // 1. Create Global Vertex Buffer
    m_globalVertexBuffer = std::make_unique<Buffer>(
        context,
        VERTEX_BUFFER_SIZE,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY
    );

    // 2. Create Global Index Buffer
    m_globalIndexBuffer = std::make_unique<Buffer>(
        context,
        INDEX_BUFFER_SIZE,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY
    );
    
    std::cout << "Global Geometry Buffers allocated." << std::endl;
}

GeometryManager::~GeometryManager() {
    // Buffers verify their destruction
}

Mesh* GeometryManager::uploadMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    VkDeviceSize vertexDataSize = vertices.size() * sizeof(Vertex);
    VkDeviceSize indexDataSize = indices.size() * sizeof(uint32_t);

    if (m_vertexOffset + vertexDataSize > VERTEX_BUFFER_SIZE) {
        throw std::runtime_error("Global Vertex Buffer full!");
    }
    if (m_indexOffset + indexDataSize > INDEX_BUFFER_SIZE) {
        throw std::runtime_error("Global Index Buffer full!");
    }

    // 1. Staging Buffers
    Buffer vertexStaging(m_context, vertexDataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    vertexStaging.upload(vertices.data(), vertexDataSize);

    Buffer indexStaging(m_context, indexDataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    indexStaging.upload(indices.data(), indexDataSize);

    // 2. Copy to Global Buffers
    VkCommandBuffer cmd = m_context.beginSingleTimeCommands();

    VkBufferCopy vertexCopy{};
    vertexCopy.srcOffset = 0;
    vertexCopy.dstOffset = m_vertexOffset;
    vertexCopy.size = vertexDataSize;
    vkCmdCopyBuffer(cmd, vertexStaging.getBuffer(), m_globalVertexBuffer->getBuffer(), 1, &vertexCopy);

    VkBufferCopy indexCopy{};
    indexCopy.srcOffset = 0;
    indexCopy.dstOffset = m_indexOffset;
    indexCopy.size = indexDataSize;
    vkCmdCopyBuffer(cmd, indexStaging.getBuffer(), m_globalIndexBuffer->getBuffer(), 1, &indexCopy);

    // 2.5. Explicit Memory Barrier (Transfer -> Vertex/Index Input)
    {
        VkBufferMemoryBarrier2 vertexBarrier{};
        vertexBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        vertexBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        vertexBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        vertexBarrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
        vertexBarrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        vertexBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vertexBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vertexBarrier.buffer = m_globalVertexBuffer->getBuffer();
        vertexBarrier.offset = m_vertexOffset;
        vertexBarrier.size = vertexDataSize;

        VkBufferMemoryBarrier2 indexBarrier{};
        indexBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        indexBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        indexBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        indexBarrier.dstStageMask = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        indexBarrier.dstAccessMask = VK_ACCESS_2_INDEX_READ_BIT;
        indexBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        indexBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        indexBarrier.buffer = m_globalIndexBuffer->getBuffer();
        indexBarrier.offset = m_indexOffset;
        indexBarrier.size = indexDataSize;

        VkDependencyInfo dependencyInfo{};
        dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        
        VkBufferMemoryBarrier2 barriers[] = {vertexBarrier, indexBarrier};
        dependencyInfo.bufferMemoryBarrierCount = 2;
        dependencyInfo.pBufferMemoryBarriers = barriers;

        vkCmdPipelineBarrier2(cmd, &dependencyInfo);
    }

    m_context.endSingleTimeCommands(cmd);

    // 3. Create Mesh Handle
    // Pass OFFSETS in ELEMENTS (not bytes) for binding?
    // vkCmdDrawIndexed(cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance)
    // firstIndex: offset in the index buffer (in elements, uint32)
    // vertexOffset: value added to the vertex index before indexing into vertex buffer.
    
    // NOTE: vkCmdBindVertexBuffers binds the WHOLE buffer with an offset.
    // If we bind global buffer at offset 0, then:
    // - vertexOffset in DrawIndexed should be: (m_vertexOffset / sizeof(Vertex))
    // - firstIndex in DrawIndexed should be: (m_indexOffset / sizeof(uint32))
    
    uint32_t firstIndex = static_cast<uint32_t>(m_indexOffset / sizeof(uint32_t));
    int32_t vertexOffset = static_cast<int32_t>(m_vertexOffset / sizeof(Vertex));
    
    Mesh* mesh = new Mesh(static_cast<uint32_t>(indices.size()), firstIndex, vertexOffset);

    // Advance offsets
    m_vertexOffset += vertexDataSize;
    m_indexOffset += indexDataSize;

    // Alignment? Vulkan requires specific alignment for buffers?
    // Typically vertex/index buffers don't need strict alignment between chunks if we use offsets, 
    // BUT binding offsets often need 4/16 byte alignment. Our sizes are multiples of struct size, which is usually aligned.
    // Vertex is 3float+3float+3float+2float = 11 floats = 44 bytes. 44 is 4-byte aligned. OK.
    
    return mesh;
}

void GeometryManager::bind(VkCommandBuffer commandBuffer) {
    VkBuffer vertexBuffers[] = {m_globalVertexBuffer->getBuffer()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
    
    vkCmdBindIndexBuffer(commandBuffer, m_globalIndexBuffer->getBuffer(), 0, VK_INDEX_TYPE_UINT32);
}

} // namespace gfx
