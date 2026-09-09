#include "runtime/PlayerPresenter.hpp"
#include <array>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace proto {
namespace {
VkShaderModule loadShader(VkDevice device, const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Missing player presenter shader: " + utf8(path.wstring()));
    const auto bytes = file.tellg();
    if (bytes <= 0 || bytes % 4 != 0)
        throw std::runtime_error("Invalid player presenter SPIR-V size");
    std::vector<uint32_t> code(static_cast<size_t>(bytes) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), bytes);
    if (!file || code.empty() || code.front() != 0x07230203)
        throw std::runtime_error("Invalid player presenter SPIR-V");
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = static_cast<size_t>(bytes);
    info.pCode = code.data();
    VkShaderModule result{};
    vkCheck(vkCreateShaderModule(device, &info, nullptr, &result), "Player presenter shader module");
    return result;
}
} // namespace

PlayerPresenter::PlayerPresenter(const VulkanContext& context, const std::filesystem::path& shaderDirectory)
    : context_(context), shaderDirectory_(shaderDirectory) {
    try {
        const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = 1;
        layout.pBindings = &binding;
        vkCheck(vkCreateDescriptorSetLayout(context_.device, &layout, nullptr, &descriptorLayout_),
                "Player presenter descriptor layout");

        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                            static_cast<uint32_t>(VulkanRenderer::framesInFlight)};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = static_cast<uint32_t>(VulkanRenderer::framesInFlight);
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &poolSize;
        vkCheck(vkCreateDescriptorPool(context_.device, &pool, nullptr, &descriptorPool_),
                "Player presenter descriptor pool");

        std::array<VkDescriptorSetLayout, VulkanRenderer::framesInFlight> layouts{};
        layouts.fill(descriptorLayout_);
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = descriptorPool_;
        allocate.descriptorSetCount = static_cast<uint32_t>(layouts.size());
        allocate.pSetLayouts = layouts.data();
        vkCheck(vkAllocateDescriptorSets(context_.device, &allocate, descriptors_.data()),
                "Player presenter descriptor sets");

        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter = VK_FILTER_LINEAR;
        sampler.minFilter = VK_FILTER_LINEAR;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = 0;
        vkCheck(vkCreateSampler(context_.device, &sampler, nullptr, &sampler_), "Player presenter sampler");
    } catch (...) {
        destroyResources();
        throw;
    }
}

PlayerPresenter::~PlayerPresenter() {
    // The owner must keep the device idle while destroying this presenter.
    destroyResources();
}

void PlayerPresenter::destroyPipeline() {
    if (pipeline_)
        vkDestroyPipeline(context_.device, pipeline_, nullptr);
    pipeline_ = {};
    if (pipelineLayout_)
        vkDestroyPipelineLayout(context_.device, pipelineLayout_, nullptr);
    pipelineLayout_ = {};
    pipelineFormat_ = {};
}

void PlayerPresenter::destroyResources() {
    destroyPipeline();
    if (sampler_)
        vkDestroySampler(context_.device, sampler_, nullptr);
    sampler_ = {};
    if (descriptorPool_)
        vkDestroyDescriptorPool(context_.device, descriptorPool_, nullptr);
    descriptorPool_ = {};
    if (descriptorLayout_)
        vkDestroyDescriptorSetLayout(context_.device, descriptorLayout_, nullptr);
    descriptorLayout_ = {};
    descriptors_.fill({});
    views_.fill({});
    generations_.fill(0);
}

void PlayerPresenter::createPipeline(VkFormat format) {
    const auto vertex = loadShader(context_.device, shaderDirectory_ / "player_present.vert.spv");
    VkShaderModule fragment{};
    try {
        fragment = loadShader(context_.device, shaderDirectory_ / "player_present.frag.spv");
    } catch (...) {
        vkDestroyShaderModule(context_.device, vertex, nullptr);
        throw;
    }
    struct ShaderCleanup {
        VkDevice device;
        VkShaderModule vertex;
        VkShaderModule fragment;
        ~ShaderCleanup() {
            vkDestroyShaderModule(device, vertex, nullptr);
            vkDestroyShaderModule(device, fragment, nullptr);
        }
    } shaderCleanup{context_.device, vertex, fragment};

    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &descriptorLayout_;
    VkPipelineLayout candidateLayout{};
    vkCheck(vkCreatePipelineLayout(context_.device, &layout, nullptr, &candidateLayout),
            "Player presenter pipeline layout");

    const VkPipelineShaderStageCreateInfo stages[] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main",
         nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment,
         "main", nullptr},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_FALSE;
    depth.depthWriteEnable = VK_FALSE;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const VkDynamicState dynamicStates[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.pNext = &rendering;
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertexInput;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &samples;
    pipeline.pDepthStencilState = &depth;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = candidateLayout;
    VkPipeline candidate{};
    try {
        vkCheck(vkCreateGraphicsPipelines(context_.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &candidate),
                "Player presenter pipeline");
    } catch (...) {
        vkDestroyPipelineLayout(context_.device, candidateLayout, nullptr);
        throw;
    }

    destroyPipeline();
    pipelineLayout_ = candidateLayout;
    pipeline_ = candidate;
    pipelineFormat_ = format;
}

void PlayerPresenter::updateDescriptor(size_t frame, VkImageView view, uint64_t generation) {
    VkDescriptorImageInfo image{sampler_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptors_[frame];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(context_.device, 1, &write, 0, nullptr);
    views_[frame] = view;
    generations_[frame] = generation;
}

void PlayerPresenter::record(VulkanRenderer& renderer, VkCommandBuffer command) {
    const size_t frame = renderer.frameIndex();
    const auto view = renderer.viewportView();
    const auto extent = renderer.viewportExtent();
    const auto output = renderer.swapExtent();
    if (!view || !extent.width || !extent.height || !output.width || !output.height ||
        renderer.context().surfaceFormat == VK_FORMAT_UNDEFINED)
        return;
    const auto format = renderer.context().surfaceFormat;
    if (!pipeline_ || pipelineFormat_ != format)
        createPipeline(format);

    const auto generation = renderer.viewportGeneration();
    if (views_[frame] != view || generations_[frame] != generation)
        updateDescriptor(frame, view, generation);

    const VkViewport viewport{0, 0, static_cast<float>(output.width), static_cast<float>(output.height), 0, 1};
    const VkRect2D scissor{{0, 0}, output};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descriptors_[frame], 0,
                            nullptr);
    vkCmdDraw(command, 3, 1, 0, 0);
}
} // namespace proto
