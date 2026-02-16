#pragma once

#include "VulkanContext.hpp"
#include "BindlessSystem.hpp"
#include <string>

namespace gfx {

class Texture {
public:
    Texture(VulkanContext& context, BindlessSystem& bindlessSystem);
    ~Texture();

    // Generates a checkerboard texture in memory
    void createCheckerboard(uint32_t width, uint32_t height);
    
    // Future: void loadFromFile(const std::string& path);

    uint32_t getID() const { return m_id; }
    VkImageView getImageView() const { return m_imageView; }
    VkSampler getSampler() const { return m_sampler; }

private:
    void createImage(uint32_t width, uint32_t height, VkFormat format, void* pixels);
    void createImageView(VkFormat format);
    void createSampler();

    VulkanContext& m_context;
    BindlessSystem& m_bindlessSystem;

    VkImage m_image = VK_NULL_HANDLE;
    VkDeviceMemory m_imageMemory = VK_NULL_HANDLE;
    VkImageView m_imageView = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;

    uint32_t m_id = 0; // Bindless ID
    bool m_created = false;
};

} // namespace gfx
