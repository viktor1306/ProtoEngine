#pragma once

#include "../core/VulkanContext.hpp"
#include "Buffer.hpp"
#include "Mesh.hpp"
#include <vector>
#include <memory>

namespace gfx {

class GeometryManager {
public:
    static constexpr VkDeviceSize VERTEX_BUFFER_SIZE = 10 * 1024 * 1024;
    static constexpr VkDeviceSize INDEX_BUFFER_SIZE  =  2 * 1024 * 1024;

    GeometryManager(VulkanContext& context);
    ~GeometryManager();

    Mesh* uploadMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
    void  bind(VkCommandBuffer commandBuffer);

private:
    VulkanContext& m_context;

    std::unique_ptr<Buffer> m_globalVertexBuffer;
    std::unique_ptr<Buffer> m_globalIndexBuffer;

    VkDeviceSize m_vertexOffset = 0;
    VkDeviceSize m_indexOffset  = 0;
};

} // namespace gfx
