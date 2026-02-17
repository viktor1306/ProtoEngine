#include "Renderer.hpp"
#include <stdexcept>
#include <iostream>
#include <array>

namespace gfx {

Renderer::Renderer(VulkanContext& context, Swapchain& swapchain,
                   core::Window& window, BindlessSystem& bindlessSystem)
    : m_context(context), m_swapchain(swapchain),
      m_window(window), m_bindlessSystem(bindlessSystem)
{
    m_commandManager     = std::make_unique<CommandManager>(context, MAX_FRAMES_IN_FLIGHT);
    m_syncManager        = std::make_unique<SyncManager>(context, MAX_FRAMES_IN_FLIGHT);
    m_renderPassProvider = std::make_unique<RenderPassProvider>(context, swapchain);
    createDescriptors();
    std::cout << "Renderer initialized (Dynamic Rendering, Sync2, BDA)." << std::endl;
}

Renderer::~Renderer() {
    vkDeviceWaitIdle(m_context.getDevice());
    vkDestroyDescriptorPool(m_context.getDevice(), m_descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(m_context.getDevice(), m_descriptorSetLayout, nullptr);
}

void Renderer::createDescriptors() {
    // Set 0: shadow map sampler
    VkDescriptorSetLayoutBinding shadowBinding{};
    shadowBinding.binding         = 0;
    shadowBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowBinding.descriptorCount = 1;
    shadowBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1; layoutInfo.pBindings = &shadowBinding;
    if (vkCreateDescriptorSetLayout(m_context.getDevice(), &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
        throw std::runtime_error("Renderer: failed to create descriptor set layout!");

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.poolSizeCount = 1; poolInfo.pPoolSizes = &poolSize; poolInfo.maxSets = 1;
    if (vkCreateDescriptorPool(m_context.getDevice(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        throw std::runtime_error("Renderer: failed to create descriptor pool!");

    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = m_descriptorPool; allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &m_descriptorSetLayout;
    if (vkAllocateDescriptorSets(m_context.getDevice(), &allocInfo, &m_descriptorSet) != VK_SUCCESS)
        throw std::runtime_error("Renderer: failed to allocate descriptor set!");

    updateDescriptorSet();
}

void Renderer::updateDescriptorSet() {
    VkDescriptorImageInfo shadowImageInfo{};
    shadowImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    shadowImageInfo.imageView   = m_renderPassProvider->getShadowImageView();
    shadowImageInfo.sampler     = m_renderPassProvider->getShadowSampler();

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet          = m_descriptorSet;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &shadowImageInfo;
    vkUpdateDescriptorSets(m_context.getDevice(), 1, &write, 0, nullptr);
}

VkCommandBuffer Renderer::beginFrame() {
    m_syncManager->waitAndResetFence(m_currentFrame);

    VkResult result = vkAcquireNextImageKHR(
        m_context.getDevice(), m_swapchain.getHandle(), UINT64_MAX,
        m_syncManager->getImageAvailableSemaphore(m_currentFrame),
        VK_NULL_HANDLE, &m_imageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return VK_NULL_HANDLE;
    }

    return m_commandManager->begin(m_currentFrame);
}

void Renderer::beginShadowPass(VkCommandBuffer cmd) {
    m_renderPassProvider->beginShadowPass(cmd);
}

void Renderer::endShadowPass(VkCommandBuffer cmd) {
    m_renderPassProvider->endShadowPass(cmd);
}

void Renderer::beginMainPass(VkCommandBuffer cmd) {
    m_renderPassProvider->beginMainPass(cmd, m_imageIndex);
}

void Renderer::endMainPass(VkCommandBuffer cmd) {
    m_renderPassProvider->endMainPass(cmd, m_imageIndex);
}

void Renderer::endFrame(VkCommandBuffer cmd) {
    m_commandManager->end(m_currentFrame);
    m_syncManager->submitFrame(cmd, m_currentFrame, m_context.getGraphicsQueue());

    bool needsRecreate = m_syncManager->presentFrame(
        m_currentFrame, m_swapchain.getHandle(), m_imageIndex, m_context.getPresentQueue());

    if (needsRecreate || m_window.isResized()) {
        m_window.resetResizedFlag();
        recreateSwapchain();
    }

    m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void Renderer::recreateSwapchain() {
    vkDeviceWaitIdle(m_context.getDevice());
    m_swapchain.recreate();
    updateDescriptorSet();
}

void Renderer::reloadShaders() {
    // Caller is responsible for destroying and recreating pipelines.
    // We just ensure the device is idle before they do so.
    vkDeviceWaitIdle(m_context.getDevice());
    std::cout << "Renderer: Device idle — safe to reload shaders." << std::endl;
}

} // namespace gfx
