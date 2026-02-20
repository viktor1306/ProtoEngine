#pragma once

#include "../core/VulkanContext.hpp"
#include "Buffer.hpp"
#include "Mesh.hpp"
#include <vector>
#include <memory>
#include <stdexcept>
#include <string>
#include <string>
#include <algorithm>
#include <mutex>

namespace gfx {

class BlockAllocator {
public:
    struct Block { VkDeviceSize offset; VkDeviceSize size; };
    std::vector<Block> freeBlocks;
    VkDeviceSize m_allocated_bytes = 0;

    BlockAllocator() = default;
    BlockAllocator(VkDeviceSize capacity) {
        if (capacity > 0) freeBlocks.push_back({0, capacity});
    }

    VkDeviceSize allocate(VkDeviceSize size, VkDeviceSize alignment = 16) {
        size = (size + alignment - 1) & ~(alignment - 1);
        for (auto it = freeBlocks.begin(); it != freeBlocks.end(); ++it) {
            if (it->size >= size) {
                VkDeviceSize offset = it->offset;
                if (it->size == size) {
                    freeBlocks.erase(it);
                } else {
                    it->offset += size;
                    it->size -= size;
                }
                m_allocated_bytes += size;
                return offset;
            }
        }
        return static_cast<VkDeviceSize>(-1);
    }

    void free(VkDeviceSize offset, VkDeviceSize size, VkDeviceSize alignment = 16) {
        size = (size + alignment - 1) & ~(alignment - 1);
        m_allocated_bytes -= size;
        freeBlocks.push_back({offset, size});
        std::sort(freeBlocks.begin(), freeBlocks.end(), [](const Block& a, const Block& b){
            return a.offset < b.offset;
        });
        
        for (size_t i = 0; i + 1 < freeBlocks.size(); ) {
            if (freeBlocks[i].offset + freeBlocks[i].size == freeBlocks[i+1].offset) {
                freeBlocks[i].size += freeBlocks[i+1].size;
                freeBlocks.erase(freeBlocks.begin() + i + 1);
            } else {
                ++i;
            }
        }
    }
    
    void reset(VkDeviceSize capacity) {
        freeBlocks.clear();
        m_allocated_bytes = 0;
        if (capacity > 0) freeBlocks.push_back({0, capacity});
    }
};

class GeometryManager {
public:
    // Buffer sizes — large enough for voxel worlds (64 MB vertex, 32 MB index)
    static constexpr VkDeviceSize VERTEX_BUFFER_SIZE = 64 * 1024 * 1024;
    static constexpr VkDeviceSize INDEX_BUFFER_SIZE  = 32 * 1024 * 1024;

    GeometryManager(VulkanContext& context);
    ~GeometryManager();

    // Upload standard gfx::Vertex mesh (existing pipeline)
    Mesh* uploadMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);

    // Structure for batched uploads
    struct UploadRequest {
        uint32_t bufferIndex;
        VkDeviceSize vertexOffset;
        VkDeviceSize indexOffset;
        VkDeviceSize vertexBytes;
        VkDeviceSize indexBytes;
        const void*  vertexData;
        const void*  indexData;
    };

    // Sub-allocate for arbitrary vertex type
    template<typename T>
    Mesh* allocateMeshRaw(uint32_t vertexCount, uint32_t indexCount, UploadRequest& outRequest, const std::vector<T>& vertices, const std::vector<uint32_t>& indices) {
        VkDeviceSize vertexDataSize = vertexCount * sizeof(T);
        VkDeviceSize indexDataSize  = indexCount  * sizeof(uint32_t);

        std::lock_guard<std::mutex> lock(m_poolMutex);
        
        uint32_t targetPoolIndex = static_cast<uint32_t>(-1);
        VkDeviceSize vOff = static_cast<VkDeviceSize>(-1);
        VkDeviceSize iOff = static_cast<VkDeviceSize>(-1);

        for (uint32_t i = 0; i < m_pools.size(); ++i) {
            vOff = m_pools[i]->vertexAllocator.allocate(vertexDataSize);
            if (vOff != static_cast<VkDeviceSize>(-1)) {
                iOff = m_pools[i]->indexAllocator.allocate(indexDataSize);
                if (iOff != static_cast<VkDeviceSize>(-1)) {
                    targetPoolIndex = i;
                    break;
                } else {
                    m_pools[i]->vertexAllocator.free(vOff, vertexDataSize);
                    vOff = static_cast<VkDeviceSize>(-1);
                }
            }
        }

        if (targetPoolIndex == static_cast<uint32_t>(-1)) {
            targetPoolIndex = allocateNewPool();
            vOff = m_pools[targetPoolIndex]->vertexAllocator.allocate(vertexDataSize);
            iOff = m_pools[targetPoolIndex]->indexAllocator.allocate(indexDataSize);
            if (vOff == static_cast<VkDeviceSize>(-1) || iOff == static_cast<VkDeviceSize>(-1)) {
                throw std::runtime_error("GeometryManager: Failed to allocate memory even in a fresh pool! Mesh is too large.");
            }
        }

        outRequest = { targetPoolIndex, vOff, iOff, vertexDataSize, indexDataSize, vertices.data(), indices.data() };

        uint32_t firstIndex   = static_cast<uint32_t>(iOff / sizeof(uint32_t));
        int32_t  vertexOffset = static_cast<int32_t>(vOff / sizeof(T));

        return new Mesh(indexCount, firstIndex, vertexOffset, targetPoolIndex);
    }

    void freeMesh(Mesh* mesh, VkDeviceSize vertexBytes, [[maybe_unused]] VkDeviceSize indexBytes) {
        if (!mesh) return;
        // In ChunkRenderer, we rely on freeMesh(int32_t, uint32_t, ...)
        delete mesh;
    }

    void freeMesh(int32_t vertexOffsetSteps, uint32_t firstIndex, VkDeviceSize vertexBytes, VkDeviceSize indexBytes, size_t vertexStride, uint32_t bufferIndex) {
        std::lock_guard<std::mutex> lock(m_poolMutex);
        if (bufferIndex < m_pools.size()) {
            m_pools[bufferIndex]->vertexAllocator.free(static_cast<VkDeviceSize>(vertexOffsetSteps) * vertexStride, vertexBytes);
            m_pools[bufferIndex]->indexAllocator.free(static_cast<VkDeviceSize>(firstIndex) * sizeof(uint32_t), indexBytes);
        }
    }

    // Execute multiple copies using a single staging buffer and memory barrier
    void executeBatchUpload(const std::vector<UploadRequest>& requests);

    // Bind a specific buffer pool
    void bindPool(VkCommandBuffer commandBuffer, uint32_t poolIndex);

    // Reset all sub-allocations and pools
    void reset();

    // Query current usage
    VkDeviceSize getVertexBytesUsed() const;
    VkDeviceSize getIndexBytesUsed()  const;

private:
    struct BufferPool {
        std::unique_ptr<Buffer> vertexBuffer;
        std::unique_ptr<Buffer> indexBuffer;
        BlockAllocator vertexAllocator;
        BlockAllocator indexAllocator;
    };

    VulkanContext& m_context;

    VkDeviceSize m_totalVertexCapacity = VERTEX_BUFFER_SIZE;
    VkDeviceSize m_totalIndexCapacity  = INDEX_BUFFER_SIZE;

    std::vector<std::unique_ptr<BufferPool>> m_pools;
    std::mutex m_poolMutex;

    uint32_t allocateNewPool();

    // Internal: stage and copy raw bytes to GPU buffers (no type knowledge)
    void uploadRawData(const void* vertexData, VkDeviceSize vertexBytes,
                       const void* indexData,  VkDeviceSize indexBytes);
};

} // namespace gfx
