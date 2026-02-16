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

    VkSwapchainKHR getHandle() const { return m_swapchain; }
    VkFormat getDateFormat() const { return m_swapchainImageFormat; }
    VkExtent2D getExtent() const { return m_swapchainExtent; }
    const std::vector<VkImageView>& getImageViews() const { return m_swapchainImageViews; }
    const std::vector<VkImage>& getImages() const { return m_swapchainImages; }
    
    // Depth resources
    VkImage getDepthImage() const { return m_depthImage; }
    VkImageView getDepthImageView() const { return m_depthImageView; }
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

    VkImage m_depthImage = VK_NULL_HANDLE;
    VkDeviceMemory m_depthImageMemory = VK_NULL_HANDLE;
    VkImageView m_depthImageView = VK_NULL_HANDLE;
    VkFormat m_depthFormat;
};

} // namespace gfx
