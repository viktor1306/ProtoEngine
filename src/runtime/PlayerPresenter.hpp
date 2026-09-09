#pragma once

#include "renderer/VulkanRenderer.hpp"
#include <array>
#include <filesystem>

namespace proto {

// Records a small, ImGui-independent fullscreen pass that presents the
// renderer's current offscreen viewport into an already active color pass.
class PlayerPresenter {
  public:
    PlayerPresenter(const VulkanContext& context, const std::filesystem::path& shaderDirectory);
    PlayerPresenter(VulkanRenderer& renderer, const std::filesystem::path& shaderDirectory)
        : PlayerPresenter(renderer.context(), shaderDirectory) {}
    ~PlayerPresenter();
    PlayerPresenter(const PlayerPresenter&) = delete;
    PlayerPresenter& operator=(const PlayerPresenter&) = delete;

    void record(VulkanRenderer& renderer, VkCommandBuffer command);

  private:
    void destroyPipeline();
    void destroyResources();
    void createPipeline(VkFormat format);
    void updateDescriptor(size_t frame, VkImageView view, uint64_t generation);

    VulkanContext context_;
    std::filesystem::path shaderDirectory_;
    VkDescriptorSetLayout descriptorLayout_{};
    VkDescriptorPool descriptorPool_{};
    std::array<VkDescriptorSet, VulkanRenderer::framesInFlight> descriptors_{};
    VkSampler sampler_{};
    VkPipelineLayout pipelineLayout_{};
    VkPipeline pipeline_{};
    VkFormat pipelineFormat_{};
    std::array<VkImageView, VulkanRenderer::framesInFlight> views_{};
    std::array<uint64_t, VulkanRenderer::framesInFlight> generations_{};
};

} // namespace proto
