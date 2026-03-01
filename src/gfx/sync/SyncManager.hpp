#pragma once

#include "gfx/core/VulkanContext.hpp"
#include <vector>

namespace gfx {

class SyncManager {
public:
    explicit SyncManager(VulkanContext& context, int framesInFlight);
    ~SyncManager();

    SyncManager(const SyncManager&)            = delete;
    SyncManager& operator=(const SyncManager&) = delete;

    void waitAndResetFence(uint32_t frameIndex);

    void submitFrame(VkCommandBuffer cmd, uint32_t frameIndex, VkQueue graphicsQueue);

    bool presentFrame(uint32_t frameIndex, VkSwapchainKHR swapchain,
                      uint32_t imageIndex, VkQueue presentQueue);

    VkSemaphore getImageAvailableSemaphore(uint32_t frameIndex) const;
    VkSemaphore getRenderFinishedSemaphore(uint32_t frameIndex) const;
    VkFence     getInFlightFence(uint32_t frameIndex)           const;

    double getWaitFenceTimeMs() const { return m_waitFenceTimeMs; }
    double getSubmitTimeMs() const    { return m_submitTimeMs; }
    double getPresentTimeMs() const   { return m_presentTimeMs; }

private:
    VulkanContext& m_context;
    int            m_framesInFlight;

    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::vector<VkFence>     m_inFlightFences;

    double m_waitFenceTimeMs = 0.0;
    double m_submitTimeMs    = 0.0;
    double m_presentTimeMs   = 0.0;
};

} // namespace gfx
