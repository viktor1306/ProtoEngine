#pragma once

#include "VulkanContext.hpp"
#include "Buffer.hpp"
#include "../core/Math.hpp"
#include <vector>

namespace gfx {

struct Vertex {
    core::math::Vec3 position;
    core::math::Vec3 normal;
    core::math::Vec3 color;
    core::math::Vec2 uv;
    float padding; // Ensure 48-byte total size (alignment safety)
};

class Mesh {
public:
    // Mesh is now just a handle to data in Global Buffer
    Mesh(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset);
    ~Mesh() = default;

    // Binds are done globally. We just draw.
    void draw(VkCommandBuffer commandBuffer);

private:
    uint32_t m_indexCount;
    uint32_t m_firstIndex;
    int32_t m_vertexOffset;
};

} // namespace gfx
