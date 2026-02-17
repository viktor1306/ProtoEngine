#pragma once

#include "gfx/core/VulkanContext.hpp"
#include <string>
#include <vector>

namespace gfx {

struct PipelineConfig {
    std::vector<VkFormat> colorAttachmentFormats;
    VkFormat depthAttachmentFormat = VK_FORMAT_UNDEFINED;

    std::string vertexShaderPath;
    std::string fragmentShaderPath;
    bool enableDepthTest = true;
    bool enableBlend     = false;
    VkPrimitiveTopology topology    = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPolygonMode       polygonMode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags     cullMode    = VK_CULL_MODE_BACK_BIT;
    VkFrontFace         frontFace   = VK_FRONT_FACE_CLOCKWISE;
    bool  depthBiasEnable   = false;
    float depthBiasConstant = 0.0f;
    float depthBiasSlope    = 0.0f;
    float depthBiasClamp    = 0.0f;

    std::vector<VkDescriptorSetLayout>          descriptorSetLayouts;
    std::vector<VkPushConstantRange>             pushConstantRanges;
    std::vector<VkVertexInputBindingDescription> bindingDescriptions;
    std::vector<VkVertexInputAttributeDescription> attributeDescriptions;
};

class Pipeline {
public:
    Pipeline(VulkanContext& context, const PipelineConfig& config);
    ~Pipeline();

    VkPipeline       getHandle() const { return m_pipeline; }
    VkPipelineLayout getLayout() const { return m_pipelineLayout; }
    void bind(VkCommandBuffer commandBuffer);

private:
    VkShaderModule createShaderModule(const std::vector<char>& code);
    static std::vector<char> readFile(const std::string& filename);

    VulkanContext&   m_context;
    VkPipeline       m_pipeline       = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
};

} // namespace gfx
