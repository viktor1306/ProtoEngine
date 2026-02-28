#pragma once

#include "VulkanContext.hpp"
#include <vector>

namespace gfx {

class Swapchain {
public:
    Swapchain(VulkanContext& context, core::Window& window);
    ~Swapchain();

    void create();
    void cleanup();
    void recreate();

    void setVSync(bool vsync);
    bool getVSync() const { return m_vsync; }

    VkSwapchainKHR getHandle() const { return m_swapchain; }
    VkFormat getImageFormat() const { return m_swapchainImageFormat; }
    VkExtent2D getExtent() const { return m_swapchainExtent; }
    const std::vector<VkImageView>& getImageViews() const { return m_swapchainImageViews; }
    const std::vector<VkImage>& getImages() const { return m_swapchainImages; }

    VkImage getDepthImage(uint32_t index) const { return m_depthImages[index]; }
    VkImageView getDepthImageView(uint32_t index) const { return m_depthImageViews[index]; }
    VkFormat getDepthFormat() const { return m_depthFormat; }

private:
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities);

    void createSwapchain();
    void createImageViews();
    void createDepthResources();

    VulkanContext& m_context;
    core::Window& m_window;

    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;
    VkFormat m_swapchainImageFormat;
    VkExtent2D m_swapchainExtent;

    bool m_vsync = false;

    std::vector<VkImage> m_depthImages;
    std::vector<VkDeviceMemory> m_depthImageMemories;
    std::vector<VkImageView> m_depthImageViews;
    VkFormat m_depthFormat;
};

} // namespace gfx
