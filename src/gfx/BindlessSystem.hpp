#pragma once

#include "VulkanContext.hpp"
#include "Buffer.hpp"
#include "../core/Math.hpp"
#include <vector>
#include <queue>

namespace gfx {

class BindlessSystem {
public:
    static constexpr uint32_t MAX_BINDLESS_RESOURCES = 1024;
    static constexpr uint32_t MAX_FRAMES = 3; // Frames in Flight (Matched with Renderer)

    struct alignas(16) ObjectDataSSBO {
        core::math::Mat4 modelMatrix;
        core::math::Vec4 color; // Optional tint
        uint32_t textureID;
        uint32_t padding[3]; // Pad to 16 bytes alignment (total 96 bytes)
    };

    BindlessSystem(VulkanContext& context);
    ~BindlessSystem();

    // Registers a texture and returns its index (ID)
    uint32_t registerTexture(VkImageView imageView, VkSampler sampler);
    
    // Unregisters a texture (frees the ID)
    void unregisterTexture(uint32_t id);

    VkDescriptorSetLayout getDescriptorSetLayout() const { return m_descriptorSetLayout; }
    
    // Bind the descriptor set for the specific frame
    void bind(VkCommandBuffer commandBuffer, VkPipelineLayout pipelineLayout, uint32_t frameIndex, uint32_t set = 1);

    // Update object data for a specific frame and object index
    void updateObject(uint32_t frameIndex, uint32_t objectIndex, const ObjectDataSSBO& data);

private:
    void createDescriptorPool();
    void createDescriptorSetLayout();
    void createDescriptorSets(); // Renamed from createDescriptorSet
    void createObjectBuffers();

    VulkanContext& m_context;
    
    VkDescriptorPool m_descriptorPool;
    VkDescriptorSetLayout m_descriptorSetLayout;
    std::vector<VkDescriptorSet> m_descriptorSets; // Per-frame sets

    // SSBO for Object Data (Double Buffered)
    Buffer* m_objectBuffers[MAX_FRAMES];
    void* m_objectBuffersMapped[MAX_FRAMES];
    size_t m_maxObjects = 10000;

    std::queue<uint32_t> m_freeIndices;
    uint32_t m_nextFreeIndex = 0;
};

} // namespace gfx
