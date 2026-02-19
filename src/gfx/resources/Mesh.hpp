#pragma once

#include "../core/VulkanContext.hpp"
#include "core/Math.hpp"
#include <vector>

namespace gfx {

struct Vertex {
    core::math::Vec3 position;
    core::math::Vec3 normal;
    core::math::Vec3 color;
    core::math::Vec2 uv;
    float padding; // 48-byte total size
};

class Mesh {
public:
    Mesh(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset);
    ~Mesh() = default;

    void draw(VkCommandBuffer commandBuffer);

    uint32_t getFirstIndex() const { return m_firstIndex; }
    int32_t  getVertexOffset() const { return m_vertexOffset; }

private:
    uint32_t m_indexCount;
    uint32_t m_firstIndex;
    int32_t  m_vertexOffset;
};

} // namespace gfx
