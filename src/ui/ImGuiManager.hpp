#pragma once

#include <vulkan/vulkan.h>
#include "gfx/core/VulkanContext.hpp"
#include "gfx/core/Swapchain.hpp"
#include "core/Window.hpp"

namespace ui {

// Manages Dear ImGui lifecycle for a WIN32 + Vulkan (Dynamic Rendering) setup.
// Usage per frame:
//   beginFrame()  — call before any ImGui::* calls
//   render(cmd)   — call inside the active vkCmdBeginRendering block (main pass)
class ImGuiManager {
public:
    ImGuiManager(gfx::VulkanContext& ctx,
                 core::Window&       window,
                 gfx::Swapchain&     swapchain);
    ~ImGuiManager();

    // Non-copyable
    ImGuiManager(const ImGuiManager&)            = delete;
    ImGuiManager& operator=(const ImGuiManager&) = delete;

    // Call once per frame BEFORE any ImGui::* widget calls.
    void beginFrame();

    // Call INSIDE an active vkCmdBeginRendering block to draw ImGui.
    void render(VkCommandBuffer cmd);

    // Call after swapchain recreation (window resize).
    void onResize(gfx::Swapchain& newSwapchain);

private:
    void createDescriptorPool();

    gfx::VulkanContext& m_context;
    core::Window&       m_window;

    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
};

} // namespace ui
