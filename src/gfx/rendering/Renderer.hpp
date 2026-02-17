#pragma once

#include "gfx/core/VulkanContext.hpp"
#include "gfx/core/Swapchain.hpp"
#include "BindlessSystem.hpp"
#include "gfx/sync/CommandManager.hpp"
#include "gfx/sync/SyncManager.hpp"
#include "RenderPassProvider.hpp"
#include <memory>

namespace gfx {

class Renderer {
public:
    static constexpr int MAX_FRAMES_IN_FLIGHT = 3;

    Renderer(VulkanContext& context, Swapchain& swapchain,
             core::Window& window, BindlessSystem& bindlessSystem);
    ~Renderer();

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    VkCommandBuffer beginFrame();
    void beginShadowPass(VkCommandBuffer cmd);
    void endShadowPass  (VkCommandBuffer cmd);
    void beginMainPass  (VkCommandBuffer cmd);
    void endMainPass    (VkCommandBuffer cmd);
    void endFrame       (VkCommandBuffer cmd);

    VkDescriptorSetLayout getDescriptorSetLayout() const { return m_descriptorSetLayout; }
    VkDescriptorSet       getDescriptorSet()       const { return m_descriptorSet; }
    uint32_t              getCurrentFrameIndex()   const { return m_currentFrame; }
    float                 getAspectRatio()         const {
        auto ext = m_swapchain.getExtent();
        return static_cast<float>(ext.width) / static_cast<float>(ext.height);
    }
    Swapchain&      getSwapchain()      { return m_swapchain; }
    BindlessSystem& getBindlessSystem() { return m_bindlessSystem; }

    void updateDescriptorSet();
    void reloadShaders();

private:
    void createDescriptors();
    void recreateSwapchain();

    VulkanContext&  m_context;
    Swapchain&      m_swapchain;
    core::Window&   m_window;
    BindlessSystem& m_bindlessSystem;

    std::unique_ptr<CommandManager>     m_commandManager;
    std::unique_ptr<SyncManager>        m_syncManager;
    std::unique_ptr<RenderPassProvider> m_renderPassProvider;

    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descriptorPool      = VK_NULL_HANDLE;
    VkDescriptorSet       m_descriptorSet       = VK_NULL_HANDLE;

    uint32_t m_currentFrame = 0;
    uint32_t m_imageIndex   = 0;
};

} // namespace gfx
