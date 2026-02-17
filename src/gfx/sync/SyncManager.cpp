#include "SyncManager.hpp"
#include <stdexcept>

namespace gfx {

SyncManager::SyncManager(VulkanContext& context, int framesInFlight)
    : m_context(context), m_framesInFlight(framesInFlight)
{
    m_imageAvailableSemaphores.resize(framesInFlight);
    m_renderFinishedSemaphores.resize(framesInFlight);
    m_inFlightFences.resize(framesInFlight);

    VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkDevice device = m_context.getDevice();
    for (int i = 0; i < framesInFlight; ++i) {
        if (vkCreateSemaphore(device, &semInfo, nullptr, &m_imageAvailableSemaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device, &semInfo, nullptr, &m_renderFinishedSemaphores[i]) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &m_inFlightFences[i]) != VK_SUCCESS)
            throw std::runtime_error("SyncManager: failed to create sync objects!");
    }
}

SyncManager::~SyncManager() {
    VkDevice device = m_context.getDevice();
    for (int i = 0; i < m_framesInFlight; ++i) {
        vkDestroySemaphore(device, m_renderFinishedSemaphores[i], nullptr);
        vkDestroySemaphore(device, m_imageAvailableSemaphores[i], nullptr);
        vkDestroyFence(device, m_inFlightFences[i], nullptr);
    }
}

void SyncManager::waitAndResetFence(uint32_t frameIndex) {
    vkWaitForFences(m_context.getDevice(), 1, &m_inFlightFences[frameIndex], VK_TRUE, UINT64_MAX);
    vkResetFences(m_context.getDevice(), 1, &m_inFlightFences[frameIndex]);
}

void SyncManager::submitFrame(VkCommandBuffer cmd, uint32_t frameIndex, VkQueue graphicsQueue) {
    VkSemaphoreSubmitInfo waitSem{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitSem.semaphore = m_imageAvailableSemaphores[frameIndex];
    waitSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalSem{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalSem.semaphore = m_renderFinishedSemaphores[frameIndex];
    signalSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = cmd;

    VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.waitSemaphoreInfoCount   = 1; submitInfo.pWaitSemaphoreInfos   = &waitSem;
    submitInfo.commandBufferInfoCount   = 1; submitInfo.pCommandBufferInfos   = &cmdInfo;
    submitInfo.signalSemaphoreInfoCount = 1; submitInfo.pSignalSemaphoreInfos = &signalSem;

    if (vkQueueSubmit2(graphicsQueue, 1, &submitInfo, m_inFlightFences[frameIndex]) != VK_SUCCESS)
        throw std::runtime_error("SyncManager: failed to submit command buffer!");
}

bool SyncManager::presentFrame(uint32_t frameIndex, VkSwapchainKHR swapchain,
                                uint32_t imageIndex, VkQueue presentQueue) {
    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores    = &m_renderFinishedSemaphores[frameIndex];
    presentInfo.swapchainCount     = 1;
    presentInfo.pSwapchains        = &swapchain;
    presentInfo.pImageIndices      = &imageIndex;

    VkResult result = vkQueuePresentKHR(presentQueue, &presentInfo);
    return (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR);
}

VkSemaphore SyncManager::getImageAvailableSemaphore(uint32_t frameIndex) const { return m_imageAvailableSemaphores[frameIndex]; }
VkSemaphore SyncManager::getRenderFinishedSemaphore(uint32_t frameIndex) const { return m_renderFinishedSemaphores[frameIndex]; }
VkFence     SyncManager::getInFlightFence(uint32_t frameIndex)           const { return m_inFlightFences[frameIndex]; }

} // namespace gfx
