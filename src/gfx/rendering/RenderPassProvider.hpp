#pragma once

#include "gfx/core/VulkanContext.hpp"
#include "gfx/core/Swapchain.hpp"

namespace gfx {

class RenderPassProvider {
public:
    static constexpr uint32_t SHADOW_WIDTH  = 2048;
    static constexpr uint32_t SHADOW_HEIGHT = 2048;

    RenderPassProvider(VulkanContext& context, Swapchain& swapchain);
    ~RenderPassProvider();

    RenderPassProvider(const RenderPassProvider&)            = delete;
    RenderPassProvider& operator=(const RenderPassProvider&) = delete;

    void beginShadowPass(VkCommandBuffer cmd, uint32_t currentFrame);
    void endShadowPass(VkCommandBuffer cmd, uint32_t currentFrame);
    void beginDepthPrePass(VkCommandBuffer cmd, uint32_t currentFrame);
    void endDepthPrePass(VkCommandBuffer cmd);
    void beginMainPass(VkCommandBuffer cmd, uint32_t swapchainImageIndex, uint32_t currentFrame);
    void endMainPass(VkCommandBuffer cmd, uint32_t swapchainImageIndex);

    VkImageView getShadowImageView(uint32_t index) const { return m_shadowImageViews[index]; }
    VkSampler getShadowSampler() const { return m_shadowSampler; }

private:
    void createShadowResources();

    VulkanContext& m_context;
    Swapchain& m_swapchain;

    std::vector<VmaAllocation> m_shadowAllocations;
    std::vector<VkImage>       m_shadowImages;
    std::vector<VkImageView>   m_shadowImageViews;
    VkSampler                  m_shadowSampler = VK_NULL_HANDLE;
};

} // namespace gfx
