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

    void beginShadowPass(VkCommandBuffer cmd);
    void endShadowPass  (VkCommandBuffer cmd);
    void beginMainPass  (VkCommandBuffer cmd, uint32_t swapchainImageIndex);
    void endMainPass    (VkCommandBuffer cmd, uint32_t swapchainImageIndex);

    VkImageView getShadowImageView() const { return m_shadowImageView; }
    VkSampler   getShadowSampler()   const { return m_shadowSampler;   }

private:
    void createShadowResources();

    VulkanContext& m_context;
    Swapchain&     m_swapchain;

    VmaAllocation m_shadowAllocation = VK_NULL_HANDLE;
    VkImage       m_shadowImage      = VK_NULL_HANDLE;
    VkImageView   m_shadowImageView  = VK_NULL_HANDLE;
    VkSampler     m_shadowSampler    = VK_NULL_HANDLE;
};

} // namespace gfx
