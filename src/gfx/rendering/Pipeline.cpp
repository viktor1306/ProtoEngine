#include "Pipeline.hpp"
#include "gfx/resources/Mesh.hpp"
#include <fstream>
#include <stdexcept>

namespace gfx {

Pipeline::Pipeline(VulkanContext& context, const PipelineConfig& config)
    : m_context(context)
{
    auto vertCode = readFile(config.vertexShaderPath);
    auto fragCode = readFile(config.fragmentShaderPath);
    VkShaderModule vertModule = createShaderModule(vertCode);
    VkShaderModule fragModule = createShaderModule(fragCode);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule; stages[0].pName = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule; stages[1].pName = "main";

    // Vertex input
    VkVertexInputBindingDescription defaultBinding{};
    std::vector<VkVertexInputAttributeDescription> defaultAttribs;
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    if (!config.bindingDescriptions.empty()) {
        vertexInput.vertexBindingDescriptionCount   = static_cast<uint32_t>(config.bindingDescriptions.size());
        vertexInput.pVertexBindingDescriptions      = config.bindingDescriptions.data();
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(config.attributeDescriptions.size());
        vertexInput.pVertexAttributeDescriptions    = config.attributeDescriptions.data();
    } else {
        defaultBinding = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        defaultAttribs = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float)*3},
            {2, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float)*6},
            {3, 0, VK_FORMAT_R32G32_SFLOAT,    sizeof(float)*9},
        };
        vertexInput.vertexBindingDescriptionCount   = 1;
        vertexInput.pVertexBindingDescriptions      = &defaultBinding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(defaultAttribs.size());
        vertexInput.pVertexAttributeDescriptions    = defaultAttribs.data();
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = config.topology;

    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1; viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = config.polygonMode; rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = config.cullMode; rasterizer.frontFace = config.frontFace;
    rasterizer.depthBiasEnable = config.depthBiasEnable ? VK_TRUE : VK_FALSE;
    rasterizer.depthBiasConstantFactor = config.depthBiasConstant;
    rasterizer.depthBiasClamp = config.depthBiasClamp;
    rasterizer.depthBiasSlopeFactor = config.depthBiasSlope;

    VkPipelineMultisampleStateCreateInfo multisampling{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable  = config.enableDepthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = config.enableDepthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = config.enableBlend ? VK_TRUE : VK_FALSE;
    if (config.enableBlend) {
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo colorBlending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlending.attachmentCount = 1; colorBlending.pAttachments = &blendAttachment;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = 2; dynamicState.pDynamicStates = dynStates;

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = static_cast<uint32_t>(config.descriptorSetLayouts.size());
    layoutInfo.pSetLayouts    = config.descriptorSetLayouts.data();

    static VkPushConstantRange defaultPCR{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, 192};
    if (!config.pushConstantRanges.empty()) {
        layoutInfo.pushConstantRangeCount = static_cast<uint32_t>(config.pushConstantRanges.size());
        layoutInfo.pPushConstantRanges    = config.pushConstantRanges.data();
    } else {
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges    = &defaultPCR;
    }

    if (vkCreatePipelineLayout(m_context.getDevice(), &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create pipeline layout!");

    VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    renderingInfo.colorAttachmentCount    = static_cast<uint32_t>(config.colorAttachmentFormats.size());
    renderingInfo.pColorAttachmentFormats = config.colorAttachmentFormats.data();
    renderingInfo.depthAttachmentFormat   = config.depthAttachmentFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.pNext               = &renderingInfo;
    pipelineInfo.stageCount          = 2; pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState   = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState      = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState   = &multisampling;
    pipelineInfo.pDepthStencilState  = &depthStencil;
    pipelineInfo.pColorBlendState    = &colorBlending;
    pipelineInfo.pDynamicState       = &dynamicState;
    pipelineInfo.layout              = m_pipelineLayout;

    if (vkCreateGraphicsPipelines(m_context.getDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS)
        throw std::runtime_error("failed to create graphics pipeline!");

    vkDestroyShaderModule(m_context.getDevice(), fragModule, nullptr);
    vkDestroyShaderModule(m_context.getDevice(), vertModule, nullptr);
}

Pipeline::~Pipeline() {
    vkDestroyPipeline(m_context.getDevice(), m_pipeline, nullptr);
    vkDestroyPipelineLayout(m_context.getDevice(), m_pipelineLayout, nullptr);
}

void Pipeline::bind(VkCommandBuffer commandBuffer) {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
}

VkShaderModule Pipeline::createShaderModule(const std::vector<char>& code) {
    VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = code.size();
    createInfo.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module;
    if (vkCreateShaderModule(m_context.getDevice(), &createInfo, nullptr, &module) != VK_SUCCESS)
        throw std::runtime_error("failed to create shader module!");
    return module;
}

std::vector<char> Pipeline::readFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("failed to open file: " + filename);
    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0); file.read(buffer.data(), fileSize);
    return buffer;
}

} // namespace gfx
