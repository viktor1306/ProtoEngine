#include "RenderPassProvider.hpp"
#include "ImageUtils.hpp"
#include <stdexcept>
#include <iostream>

namespace gfx {

RenderPassProvider::RenderPassProvider(VulkanContext& context, Swapchain& swapchain)
    : m_context(context), m_swapchain(swapchain)
{
    std::cout << "RenderPassProvider: Creating shadow resources (VMA)..." << std::endl;
    createShadowResources();
}

RenderPassProvider::~RenderPassProvider() {
    VkDevice device = m_context.getDevice();
    if (m_shadowSampler   != VK_NULL_HANDLE) { vkDestroySampler(device, m_shadowSampler, nullptr);   m_shadowSampler   = VK_NULL_HANDLE; }
    for (auto view : m_shadowImageViews) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
    }
    for (size_t i = 0; i < m_shadowImages.size(); ++i) {
        if (m_shadowImages[i] != VK_NULL_HANDLE && m_shadowAllocations[i] != VK_NULL_HANDLE) {
            vmaDestroyImage(m_context.getAllocator(), m_shadowImages[i], m_shadowAllocations[i]);
        }
    }
    m_shadowImageViews.clear();
    m_shadowImages.clear();
    m_shadowAllocations.clear();
}

void RenderPassProvider::createShadowResources() {
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType   = VK_IMAGE_TYPE_2D;
    imageInfo.extent      = {SHADOW_WIDTH, SHADOW_HEIGHT, 1};
    imageInfo.mipLevels   = 1; imageInfo.arrayLayers = 1;
    imageInfo.format      = VK_FORMAT_D32_SFLOAT;
    imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples     = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{}; allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    size_t count = 3; // MAX_FRAMES_IN_FLIGHT
    m_shadowImages.resize(count);
    m_shadowAllocations.resize(count);
    m_shadowImageViews.resize(count);

    for (size_t i = 0; i < count; i++) {
        if (vmaCreateImage(m_context.getAllocator(), &imageInfo, &allocInfo, &m_shadowImages[i], &m_shadowAllocations[i], nullptr) != VK_SUCCESS)
            throw std::runtime_error("RenderPassProvider: failed to create shadow image via VMA!");

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = m_shadowImages[i]; viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(m_context.getDevice(), &viewInfo, nullptr, &m_shadowImageViews[i]) != VK_SUCCESS)
            throw std::runtime_error("RenderPassProvider: failed to create shadow image view!");
    }

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.maxAnisotropy = 1.0f; samplerInfo.maxLod = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (vkCreateSampler(m_context.getDevice(), &samplerInfo, nullptr, &m_shadowSampler) != VK_SUCCESS)
        throw std::runtime_error("RenderPassProvider: failed to create shadow sampler!");
}

void RenderPassProvider::beginShadowPass(VkCommandBuffer cmd, uint32_t currentFrame) {
    utils::transitionImageLayout(cmd, m_shadowImages[currentFrame],
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView   = m_shadowImageViews[currentFrame];
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea = {{0,0},{SHADOW_WIDTH,SHADOW_HEIGHT}};
    renderingInfo.layerCount = 1; renderingInfo.pDepthAttachment = &depthAtt;
    vkCmdBeginRendering(cmd, &renderingInfo);

    VkViewport vp{0,0,(float)SHADOW_WIDTH,(float)SHADOW_HEIGHT,0,1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{{0,0},{SHADOW_WIDTH,SHADOW_HEIGHT}};
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void RenderPassProvider::endShadowPass(VkCommandBuffer cmd, uint32_t currentFrame) {
    vkCmdEndRendering(cmd);
    utils::transitionImageLayout(cmd, m_shadowImages[currentFrame],
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
}

void RenderPassProvider::beginDepthPrePass(VkCommandBuffer cmd, uint32_t currentFrame) {
    // Transition depth image for pre-pass
    utils::transitionImageLayout(cmd, m_swapchain.getDepthImage(currentFrame),
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);

    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView   = m_swapchain.getDepthImageView(currentFrame);
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.clearValue.depthStencil = {1.0f, 0};

    VkExtent2D extent = m_swapchain.getExtent();
    VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea = {{0,0},extent}; renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 0; // No color write
    renderingInfo.pDepthAttachment = &depthAtt;
    vkCmdBeginRendering(cmd, &renderingInfo);

    VkViewport vp{0,0,(float)extent.width,(float)extent.height,0,1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{{0,0},extent};
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void RenderPassProvider::endDepthPrePass(VkCommandBuffer cmd) {
    vkCmdEndRendering(cmd);
}

void RenderPassProvider::beginMainPass(VkCommandBuffer cmd, uint32_t swapchainImageIndex, uint32_t currentFrame) {
    utils::transitionImageLayout(cmd, m_swapchain.getImages()[swapchainImageIndex],
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    // Depth image is already in DEPTH_STENCIL_ATTACHMENT_OPTIMAL from Depth Pre-Pass. No transition needed.

    VkRenderingAttachmentInfo colorAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAtt.imageView   = m_swapchain.getImageViews()[swapchainImageIndex];
    colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.clearValue.color = {{0.5f,0.7f,1.0f,1.0f}};

    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView   = m_swapchain.getDepthImageView(currentFrame);
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD; // LOAD instread of CLEAR
    depthAtt.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.clearValue.depthStencil = {1.0f, 0};

    VkExtent2D extent = m_swapchain.getExtent();
    VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
    renderingInfo.renderArea = {{0,0},extent}; renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1; renderingInfo.pColorAttachments = &colorAtt;
    renderingInfo.pDepthAttachment = &depthAtt;
    vkCmdBeginRendering(cmd, &renderingInfo);

    VkViewport vp{0,0,(float)extent.width,(float)extent.height,0,1};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc{{0,0},extent};
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

void RenderPassProvider::endMainPass(VkCommandBuffer cmd, uint32_t swapchainImageIndex) {
    vkCmdEndRendering(cmd);
    utils::transitionImageLayout(cmd, m_swapchain.getImages()[swapchainImageIndex],
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);
}

} // namespace gfx
