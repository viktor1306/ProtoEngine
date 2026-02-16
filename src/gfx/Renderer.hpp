#pragma once

#include "VulkanContext.hpp"
#include "Swapchain.hpp"
#include "BindlessSystem.hpp"
#include <vector>
#include <array>

namespace gfx {

class Renderer {
public:
    static constexpr int MAX_FRAMES_IN_FLIGHT = 3;

    Renderer(VulkanContext& context, Swapchain& swapchain, core::Window& window, BindlessSystem& bindlessSystem);
    ~Renderer();

    VkCommandBuffer beginFrame();
    void beginShadowPass(VkCommandBuffer commandBuffer);
    void endShadowPass(VkCommandBuffer commandBuffer);
    void beginMainPass(VkCommandBuffer commandBuffer);
    void endMainPass(VkCommandBuffer commandBuffer);
    void endFrame(VkCommandBuffer commandBuffer);

    VkDescriptorSetLayout getDescriptorSetLayout() const { return m_descriptorSetLayout; }
    VkDescriptorSet getDescriptorSet() const { return m_descriptorSet; }
    
    // int getCurrentFrameIndex() const { return m_currentFrame; } // Removed duplicate
    float getAspectRatio() const { return static_cast<float>(m_swapchain.getExtent().width) / static_cast<float>(m_swapchain.getExtent().height); }
    Swapchain& getSwapchain() { return m_swapchain; }

    void updateDescriptorSet(); // Helper to bind shadow map
    uint32_t getCurrentFrameIndex() const { return m_currentFrame; }

    // Bindless System
    BindlessSystem& getBindlessSystem() { return m_bindlessSystem; }

    void reloadShaders(); // Hot-Reload support

private:
    void recreateSwapchain();
    void cleanupSwapchain(); 

    void createCommandBuffers();
    void createSyncObjects();
    
    // Shadow Mapping
    void createShadowResources();
    void createDescriptors();

    void transitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkImageAspectFlags aspectMask);

    VulkanContext& m_context;
    Swapchain& m_swapchain;
    core::Window& m_window;
    BindlessSystem& m_bindlessSystem; // New member

    // No RenderPass or Framebuffers
    // VkRenderPass m_renderPass; 
    // std::vector<VkFramebuffer> m_framebuffers;

    // Shadow Resources
    uint32_t m_shadowWidth = 2048;
    uint32_t m_shadowHeight = 2048;
    VkImage m_shadowImage;
    VkDeviceMemory m_shadowImageMemory;
    VkImageView m_shadowImageView;
    VkSampler m_shadowSampler;
    
    // Descriptors
    VkDescriptorSetLayout m_descriptorSetLayout;
    VkDescriptorPool m_descriptorPool;
    VkDescriptorSet m_descriptorSet;

    std::vector<VkCommandBuffer> m_commandBuffers;

    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::vector<VkFence> m_inFlightFences;

    uint32_t m_currentFrame = 0;
    uint32_t m_imageIndex = 0;
};

} // namespace gfx
