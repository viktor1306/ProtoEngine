#include "CommandManager.hpp"
#include <stdexcept>

namespace gfx {

CommandManager::CommandManager(VulkanContext& context, int framesInFlight)
    : m_context(context), m_framesInFlight(framesInFlight)
{
    m_commandBuffers.resize(framesInFlight);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = m_context.getCommandPool();
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(framesInFlight);
    if (vkAllocateCommandBuffers(m_context.getDevice(), &allocInfo, m_commandBuffers.data()) != VK_SUCCESS)
        throw std::runtime_error("CommandManager: failed to allocate command buffers!");
}

CommandManager::~CommandManager() {
    vkFreeCommandBuffers(m_context.getDevice(), m_context.getCommandPool(),
                         static_cast<uint32_t>(m_commandBuffers.size()), m_commandBuffers.data());
}

VkCommandBuffer CommandManager::begin(uint32_t frameIndex) {
    VkCommandBuffer cmd = m_commandBuffers[frameIndex];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS)
        throw std::runtime_error("CommandManager: failed to begin command buffer!");
    return cmd;
}

void CommandManager::end(uint32_t frameIndex) {
    if (vkEndCommandBuffer(m_commandBuffers[frameIndex]) != VK_SUCCESS)
        throw std::runtime_error("CommandManager: failed to end command buffer!");
}

VkCommandBuffer CommandManager::get(uint32_t frameIndex) const {
    return m_commandBuffers[frameIndex];
}

} // namespace gfx
