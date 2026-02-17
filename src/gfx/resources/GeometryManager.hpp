#pragma once

#include "../core/VulkanContext.hpp"
#include "Buffer.hpp"
#include "Mesh.hpp"
#include <vector>
#include <memory>
#include <stdexcept>
#include <string>

namespace gfx {

class GeometryManager {
public:
    // Buffer sizes — large enough for voxel worlds (64 MB vertex, 32 MB index)
    static constexpr VkDeviceSize VERTEX_BUFFER_SIZE = 64 * 1024 * 1024;
    static constexpr VkDeviceSize INDEX_BUFFER_SIZE  = 32 * 1024 * 1024;

    GeometryManager(VulkanContext& context);
    ~GeometryManager();

    // Upload standard gfx::Vertex mesh (existing pipeline)
    Mesh* uploadMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);

    // Upload arbitrary vertex type (e.g. world::VoxelVertex — 8 bytes)
    // The vertex stride is sizeof(T); Mesh stores firstIndex and vertexOffset
    // in terms of element count (not bytes), so the caller must bind the
    // correct VkVertexInputBindingDescription for type T.
    template<typename T>
    Mesh* uploadMeshRaw(const std::vector<T>& vertices, const std::vector<uint32_t>& indices) {
        VkDeviceSize vertexDataSize = vertices.size() * sizeof(T);
        VkDeviceSize indexDataSize  = indices.size()  * sizeof(uint32_t);

        if (m_vertexOffset + vertexDataSize > VERTEX_BUFFER_SIZE)
            throw std::runtime_error("GeometryManager: Vertex buffer full! (" + std::to_string(vertexDataSize) + " bytes needed)");
        if (m_indexOffset + indexDataSize > INDEX_BUFFER_SIZE)
            throw std::runtime_error("GeometryManager: Index buffer full! (" + std::to_string(indexDataSize) + " bytes needed)");

        uploadRawData(vertices.data(), vertexDataSize, indices.data(), indexDataSize);

        // firstIndex: index into the index buffer (in uint32_t units)
        uint32_t firstIndex   = static_cast<uint32_t>(m_indexOffset  / sizeof(uint32_t));
        // vertexOffset: byte offset into vertex buffer (passed to vkCmdDrawIndexed as vertexOffset)
        // For non-standard vertex sizes we store the byte offset and the pipeline
        // must use the correct stride. We cast to int32_t for Mesh compatibility.
        int32_t  vertexOffset = static_cast<int32_t>(m_vertexOffset / sizeof(T));

        m_vertexOffset += vertexDataSize;
        m_indexOffset  += indexDataSize;

        return new Mesh(static_cast<uint32_t>(indices.size()), firstIndex, vertexOffset);
    }

    // Bind the global vertex + index buffers (works for any vertex type —
    // the pipeline's VkVertexInputBindingDescription defines the stride).
    void bind(VkCommandBuffer commandBuffer);

    // Reset buffer offsets to 0 so the GPU buffers can be reused from scratch.
    // All previously returned Mesh* pointers become invalid after this call.
    // Caller must ensure GPU is idle (vkDeviceWaitIdle) before calling.
    void reset();

    // Query current usage
    VkDeviceSize getVertexBytesUsed() const { return m_vertexOffset; }
    VkDeviceSize getIndexBytesUsed()  const { return m_indexOffset; }

private:
    VulkanContext& m_context;

    std::unique_ptr<Buffer> m_globalVertexBuffer;
    std::unique_ptr<Buffer> m_globalIndexBuffer;

    VkDeviceSize m_vertexOffset = 0;
    VkDeviceSize m_indexOffset  = 0;

    // Internal: stage and copy raw bytes to GPU buffers (no type knowledge)
    void uploadRawData(const void* vertexData, VkDeviceSize vertexBytes,
                       const void* indexData,  VkDeviceSize indexBytes);
};

} // namespace gfx
