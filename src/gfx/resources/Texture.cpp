#include "Texture.hpp"
#include <stdexcept>
#include <vector>

namespace gfx {

Texture::Texture(VulkanContext& context, BindlessSystem& bindlessSystem)
    : m_context(context), m_bindlessSystem(bindlessSystem) {}

Texture::~Texture() {
    if (m_created) m_bindlessSystem.unregisterTexture(m_id);
    VkDevice dev = m_context.getDevice();
    if (m_sampler    != VK_NULL_HANDLE) vkDestroySampler(dev, m_sampler, nullptr);
    if (m_imageView  != VK_NULL_HANDLE) vkDestroyImageView(dev, m_imageView, nullptr);
    if (m_image      != VK_NULL_HANDLE) vkDestroyImage(dev, m_image, nullptr);
    if (m_imageMemory!= VK_NULL_HANDLE) vkFreeMemory(dev, m_imageMemory, nullptr);
}

void Texture::createCheckerboard(uint32_t width, uint32_t height) {
    if (m_created) throw std::runtime_error("Texture already created!");
    std::vector<uint8_t> pixels(width * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            bool isWhite = ((x / 32) + (y / 32)) % 2 == 1;
            uint8_t* p = &pixels[(y * width + x) * 4];
            p[0] = isWhite ? 200 : 50;
            p[1] = isWhite ? 200 : 50;
            p[2] = isWhite ? 255 : 50;
            p[3] = 255;
        }
    }
    createImage(width, height, VK_FORMAT_R8G8B8A8_SRGB, pixels.data());
    createImageView(VK_FORMAT_R8G8B8A8_SRGB);
    createSampler();
    m_id = m_bindlessSystem.registerTexture(m_imageView, m_sampler);
    m_created = true;
}

void Texture::createImage(uint32_t width, uint32_t height, VkFormat format, void* pixels) {
    VkDeviceSize imageSize = width * height * 4;
    VkBuffer stagingBuffer; VkDeviceMemory stagingMemory;
    m_context.createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer, stagingMemory);
    void* data;
    vkMapMemory(m_context.getDevice(), stagingMemory, 0, imageSize, 0, &data);
    memcpy(data, pixels, static_cast<size_t>(imageSize));
    vkUnmapMemory(m_context.getDevice(), stagingMemory);

    m_context.createImage(width, height, format, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_image, m_imageMemory);

    VkCommandBuffer cmd = m_context.beginSingleTimeCommands();

    // UNDEFINED → TRANSFER_DST
    VkImageMemoryBarrier2 b1{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b1.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT; b1.srcAccessMask = 0;
    b1.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b1.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.srcQueueFamilyIndex = b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.image = m_image; b1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep1.imageMemoryBarrierCount = 1; dep1.pImageMemoryBarriers = &b1;
    vkCmdPipelineBarrier2(cmd, &dep1);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuffer, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // TRANSFER_DST → SHADER_READ
    VkImageMemoryBarrier2 b2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b2.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; b2.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b2.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; b2.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b2.srcQueueFamilyIndex = b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.image = m_image; b2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep2.imageMemoryBarrierCount = 1; dep2.pImageMemoryBarriers = &b2;
    vkCmdPipelineBarrier2(cmd, &dep2);

    m_context.endSingleTimeCommands(cmd);
    vkDestroyBuffer(m_context.getDevice(), stagingBuffer, nullptr);
    vkFreeMemory(m_context.getDevice(), stagingMemory, nullptr);
}

void Texture::createImageView(VkFormat format) {
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = m_image; viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D; viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(m_context.getDevice(), &viewInfo, nullptr, &m_imageView) != VK_SUCCESS)
        throw std::runtime_error("failed to create texture image view!");
}

void Texture::createSampler() {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_context.getPhysicalDevice(), &props);
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR; samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_TRUE; samplerInfo.maxAnisotropy = props.limits.maxSamplerAnisotropy;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; samplerInfo.maxLod = 100.0f;
    if (vkCreateSampler(m_context.getDevice(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS)
        throw std::runtime_error("failed to create texture sampler!");
}

} // namespace gfx
