#pragma once

#include "VulkanContext.hpp"
#include "Buffer.hpp"
#include "Mesh.hpp" // For Vertex definition
#include <vector>
#include <memory>

namespace gfx {

class GeometryManager {
public:
    // Default size: 64MB Vertices, 16MB Indices ?
    // Let's start smaller for testing: 10MB / 2MB
    static constexpr VkDeviceSize VERTEX_BUFFER_SIZE = 10 * 1024 * 1024; 
    static constexpr VkDeviceSize INDEX_BUFFER_SIZE = 2 * 1024 * 1024;

    GeometryManager(VulkanContext& context);
    ~GeometryManager();

    // Uploads mesh data and returns a Mesh object (by value or pointer)
    // The returned Mesh will contain offsets into the global buffer.
    Mesh* uploadMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);

    // Binds the global buffers to the command buffer
    void bind(VkCommandBuffer commandBuffer);

private:
    VulkanContext& m_context;
    
    std::unique_ptr<Buffer> m_globalVertexBuffer;
    std::unique_ptr<Buffer> m_globalIndexBuffer;

    VkDeviceSize m_vertexOffset = 0; // In BYTES
    VkDeviceSize m_indexOffset = 0;  // In BYTES
    
    uint32_t m_vertexCount = 0; // Total vertices count (for offset calculation in elements)
    uint32_t m_indexCount = 0;  // Total indices count
};

} // namespace gfx
