#include "ImGuiManager.hpp"

// ImGui core
#include "imgui.h"
// WIN32 backend (no GLFW — we use raw Win32)
#include "backends/imgui_impl_win32.h"
// Vulkan backend
#include "backends/imgui_impl_vulkan.h"

#include <stdexcept>
#include <array>

namespace ui {

ImGuiManager::ImGuiManager(gfx::VulkanContext& ctx,
                           core::Window&       window,
                           gfx::Swapchain&     swapchain)
    : m_context(ctx)
    , m_window(window)
{
    // 1. Create descriptor pool with FREE_DESCRIPTOR_SET_BIT so ImGui can
    //    manage its own descriptors (required by imgui_impl_vulkan).
    createDescriptorPool();

    // 2. ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // 3. Style
    ImGui::StyleColorsDark();

    // 4. WIN32 backend
    ImGui_ImplWin32_Init(window.getHandle());

    // 5. Vulkan backend — Dynamic Rendering path (no VkRenderPass)
    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance        = ctx.getInstance();
    initInfo.PhysicalDevice  = ctx.getPhysicalDevice();
    initInfo.Device          = ctx.getDevice();
    initInfo.QueueFamily     = ctx.findQueueFamilies(ctx.getPhysicalDevice()).graphicsFamily.value();
    initInfo.Queue           = ctx.getGraphicsQueue();
    initInfo.DescriptorPool  = m_descriptorPool;
    initInfo.MinImageCount   = 2;
    initInfo.ImageCount      = static_cast<uint32_t>(swapchain.getImages().size());
    initInfo.MSAASamples     = VK_SAMPLE_COUNT_1_BIT;
    initInfo.RenderPass      = VK_NULL_HANDLE; // Dynamic Rendering — no render pass

    // Dynamic Rendering pipeline info — must match the active render pass formats exactly.
    // ImGui is rendered inside the main pass which has both color AND depth attachments.
    VkFormat colorFmt = swapchain.getImageFormat();
    VkFormat depthFmt = swapchain.getDepthFormat();
    initInfo.UseDynamicRendering = true;
    initInfo.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    initInfo.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
    initInfo.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFmt;
    initInfo.PipelineRenderingCreateInfo.depthAttachmentFormat   = depthFmt;

    if (!ImGui_ImplVulkan_Init(&initInfo))
        throw std::runtime_error("ImGui_ImplVulkan_Init failed!");

    // 6. Upload fonts via a single-time command buffer
    VkCommandBuffer cmd = ctx.beginSingleTimeCommands();
    ImGui_ImplVulkan_CreateFontsTexture();
    ctx.endSingleTimeCommands(cmd);
}

ImGuiManager::~ImGuiManager() {
    vkDeviceWaitIdle(m_context.getDevice());
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(m_context.getDevice(), m_descriptorPool, nullptr);
}

void ImGuiManager::createDescriptorPool() {
    // ImGui needs a pool with VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT
    // so it can allocate and free its own descriptor sets independently.
    std::array<VkDescriptorPoolSize, 1> poolSizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}
    }};

    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; // Required by ImGui
    poolInfo.maxSets       = 16;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();

    if (vkCreateDescriptorPool(m_context.getDevice(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        throw std::runtime_error("Failed to create ImGui descriptor pool!");
}

void ImGuiManager::beginFrame() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ImGuiManager::render(VkCommandBuffer cmd) {
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

void ImGuiManager::onResize(gfx::Swapchain& newSwapchain) {
    // Notify ImGui of the new image count (swapchain may have changed)
    ImGui_ImplVulkan_SetMinImageCount(static_cast<uint32_t>(newSwapchain.getImages().size()));
}

} // namespace ui
