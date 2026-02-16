#include "Mesh.hpp"
#include <cstring>

namespace gfx {

Mesh::Mesh(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset)
    : m_indexCount(indexCount), m_firstIndex(firstIndex), m_vertexOffset(vertexOffset) {}

void Mesh::draw(VkCommandBuffer commandBuffer) {
    vkCmdDrawIndexed(commandBuffer, m_indexCount, 1, m_firstIndex, m_vertexOffset, 0);
}

} // namespace gfx
