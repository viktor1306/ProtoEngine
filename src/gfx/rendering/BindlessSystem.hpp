#pragma once

#include "gfx/core/VulkanContext.hpp"
#include "gfx/resources/Buffer.hpp"
#include "core/Math.hpp"
#include <vector>
#include <queue>

namespace gfx {

class BindlessSystem {
public:
    static constexpr uint32_t MAX_BINDLESS_RESOURCES = 1024;
    static constexpr uint32_t MAX_FRAMES = 3;

    struct alignas(16) ObjectDataSSBO {
        core::math::Mat4 modelMatrix;
        core::math::Vec4 color;
        uint32_t textureID;
        uint32_t padding[3];
    };

    BindlessSystem(VulkanContext& context);
    ~BindlessSystem();

    uint32_t registerTexture(VkImageView imageView, VkSampler sampler);
    void     unregisterTexture(uint32_t id);

    VkDescriptorSetLayout getDescriptorSetLayout() const { return m_descriptorSetLayout; }

    void bind(VkCommandBuffer commandBuffer, VkPipelineLayout pipelineLayout,
              uint32_t frameIndex, uint32_t set = 1);

    void updateObject(uint32_t frameIndex, uint32_t objectIndex, const ObjectDataSSBO& data);

private:
    void createDescriptorPool();
    void createDescriptorSetLayout();
    void createDescriptorSets();
    void createObjectBuffers();

    VulkanContext& m_context;

    VkDescriptorPool      m_descriptorPool      = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descriptorSets;

    Buffer* m_objectBuffers[MAX_FRAMES]       = {};
    void*   m_objectBuffersMapped[MAX_FRAMES] = {};
    size_t  m_maxObjects = 10000;

    std::queue<uint32_t> m_freeIndices;
    uint32_t m_nextFreeIndex = 0;
};

} // namespace gfx
