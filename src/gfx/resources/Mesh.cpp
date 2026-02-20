#include "Mesh.hpp"

namespace gfx {

Mesh::Mesh(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t bufferIndex)
    : m_indexCount(indexCount), m_firstIndex(firstIndex), m_vertexOffset(vertexOffset), m_bufferIndex(bufferIndex) {}

void Mesh::draw(VkCommandBuffer commandBuffer) {
    vkCmdDrawIndexed(commandBuffer, m_indexCount, 1, m_firstIndex, m_vertexOffset, 0);
}

} // namespace gfx
