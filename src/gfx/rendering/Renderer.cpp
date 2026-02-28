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
    
    // Create Query Pool for GPU Timing
    VkQueryPoolCreateInfo queryPoolInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryPoolInfo.queryCount = MAX_FRAMES_IN_FLIGHT * 2;
    if (vkCreateQueryPool(m_context.getDevice(), &queryPoolInfo, nullptr, &m_queryPool) != VK_SUCCESS) {
        std::cerr << "Renderer: failed to create timestamp query pool!\n";
    }

    std::cout << "Renderer initialized (Dynamic Rendering, Sync2, BDA)." << std::endl;
}

Renderer::~Renderer() {
    vkDeviceWaitIdle(m_context.getDevice());
    if (m_queryPool != VK_NULL_HANDLE) { vkDestroyQueryPool(m_context.getDevice(), m_queryPool, nullptr); m_queryPool = VK_NULL_HANDLE; }
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

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT)};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.poolSizeCount = 1; poolInfo.pPoolSizes = &poolSize; poolInfo.maxSets = MAX_FRAMES_IN_FLIGHT;
    if (vkCreateDescriptorPool(m_context.getDevice(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        throw std::runtime_error("Renderer: failed to create descriptor pool!");

    std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, m_descriptorSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorPool = m_descriptorPool; allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocInfo.pSetLayouts = layouts.data();
    m_descriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
    if (vkAllocateDescriptorSets(m_context.getDevice(), &allocInfo, m_descriptorSets.data()) != VK_SUCCESS)
        throw std::runtime_error("Renderer: failed to allocate descriptor set!");

    updateDescriptorSet();
}

void Renderer::updateDescriptorSet() {
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> imageInfos(MAX_FRAMES_IN_FLIGHT);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos[i].imageView   = m_renderPassProvider->getShadowImageView(i);
        imageInfos[i].sampler     = m_renderPassProvider->getShadowSampler();

        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet          = m_descriptorSets[i];
        write.dstBinding      = 0;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo      = &imageInfos[i];
        writes.push_back(write);
    }
    vkUpdateDescriptorSets(m_context.getDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
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

    VkCommandBuffer cmd = m_commandManager->begin(m_currentFrame);

    if (cmd && m_queryPool != VK_NULL_HANDLE) {
        // Fetch GPU Time (from previous frame matching currentFrame)
        uint64_t timestamps[2] = {0, 0};
        VkResult res = vkGetQueryPoolResults(m_context.getDevice(), m_queryPool, m_currentFrame * 2, 2, 
                                             sizeof(uint64_t) * 2, timestamps, sizeof(uint64_t), 
                                             VK_QUERY_RESULT_64_BIT);
        if (res == VK_SUCCESS) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(m_context.getPhysicalDevice(), &props);
            float period = props.limits.timestampPeriod;
            m_gpuFrameTimeMs = static_cast<double>(timestamps[1] - timestamps[0]) * period * 1e-6;
        }

        // Reset and write start timestamp
        vkCmdResetQueryPool(cmd, m_queryPool, m_currentFrame * 2, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queryPool, m_currentFrame * 2);
    }

    return cmd;
}

void Renderer::beginShadowPass(VkCommandBuffer cmd) {
    m_renderPassProvider->beginShadowPass(cmd, m_currentFrame);
}

void Renderer::endShadowPass(VkCommandBuffer cmd) {
    m_renderPassProvider->endShadowPass(cmd, m_currentFrame);
}

void Renderer::beginDepthPrePass(VkCommandBuffer cmd) {
    m_renderPassProvider->beginDepthPrePass(cmd, m_currentFrame);
}

void Renderer::endDepthPrePass(VkCommandBuffer cmd) {
    m_renderPassProvider->endDepthPrePass(cmd);
}

void Renderer::beginMainPass(VkCommandBuffer cmd) {
    m_renderPassProvider->beginMainPass(cmd, m_imageIndex, m_currentFrame);
}

void Renderer::endMainPass(VkCommandBuffer cmd) {
    m_renderPassProvider->endMainPass(cmd, m_imageIndex);
}

void Renderer::endFrame(VkCommandBuffer cmd) {
    if (m_queryPool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queryPool, m_currentFrame * 2 + 1);
    }

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
