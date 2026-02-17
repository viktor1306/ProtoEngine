#pragma once

#include "gfx/core/VulkanContext.hpp"
#include <vector>

namespace gfx {

class CommandManager {
public:
    explicit CommandManager(VulkanContext& context, int framesInFlight);
    ~CommandManager();

    CommandManager(const CommandManager&)            = delete;
    CommandManager& operator=(const CommandManager&) = delete;

    VkCommandBuffer begin(uint32_t frameIndex);
    void            end(uint32_t frameIndex);
    VkCommandBuffer get(uint32_t frameIndex) const;

private:
    VulkanContext&               m_context;
    int                          m_framesInFlight;
    std::vector<VkCommandBuffer> m_commandBuffers;
};

} // namespace gfx
