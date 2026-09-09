#include "renderer/VulkanRenderer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace proto {
void VulkanRenderer::createGeometry() {
    const auto geometry = makePrimitives();
    meshRanges_ = geometry.ranges;
    indexOffset_ = geometry.vertices.size() * sizeof(PrimitiveVertex);
    const VkDeviceSize bytes = indexOffset_ + geometry.indices.size() * sizeof(uint32_t);
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VmaAllocationCreateInfo memory{};
    memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    memory.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo allocated{};
    vkCheck(vmaCreateBuffer(allocator_, &info, &memory, &geometry_, &geometryMemory_, &allocated),
            "Shared primitive geometry");
    std::memcpy(allocated.pMappedData, geometry.vertices.data(), static_cast<size_t>(indexOffset_));
    std::memcpy(static_cast<char*>(allocated.pMappedData) + indexOffset_, geometry.indices.data(),
                geometry.indices.size() * sizeof(uint32_t));
    vkCheck(vmaFlushAllocation(allocator_, geometryMemory_, 0, VK_WHOLE_SIZE), "Flush primitive geometry");
}
void VulkanRenderer::createScenePipelines(const std::filesystem::path& directory) {
    struct Shader {
        VkDevice device;
        VkShaderModule handle{};
        Shader(VkDevice owner, const std::filesystem::path& path) : device(owner) {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                throw std::runtime_error("Missing scene shader");
            const auto size = file.tellg();
            if (size <= 0 || size % 4 != 0)
                throw std::runtime_error("Invalid scene SPIR-V size");
            std::vector<uint32_t> data(static_cast<size_t>(size) / 4);
            file.seekg(0);
            file.read(reinterpret_cast<char*>(data.data()), size);
            if (!file)
                throw std::runtime_error("Cannot read scene shader");
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = static_cast<size_t>(size);
            info.pCode = data.data();
            vkCheck(vkCreateShaderModule(device, &info, nullptr, &handle), "Scene shader module");
        }
        ~Shader() {
            if (handle)
                vkDestroyShaderModule(device, handle, nullptr);
        }
    } vertex(context_.device, directory / "scene.vert.spv"), fragment(context_.device, directory / "scene.frag.spv");
    VkPushConstantRange camera{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &camera;
    vkCheck(vkCreatePipelineLayout(context_.device, &layout, nullptr, &sceneLayout_), "Scene pipeline layout");
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& s : stages) {
        s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex.handle;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment.handle;
    const VkVertexInputBindingDescription bindings[]{{0, sizeof(PrimitiveVertex), VK_VERTEX_INPUT_RATE_VERTEX},
                                                     {1, sizeof(SceneInstance), VK_VERTEX_INPUT_RATE_INSTANCE}};
    std::array<VkVertexInputAttributeDescription, 7> attributes{};
    attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PrimitiveVertex, position)};
    attributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PrimitiveVertex, color)};
    for (uint32_t i = 0; i < 4; ++i)
        attributes[i + 2] = {i + 2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16};
    attributes[6] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SceneInstance, color)};
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount = 2;
    input.pVertexBindingDescriptions = bindings;
    input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    input.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = depth.depthWriteEnable = VK_TRUE;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const VkDynamicState dynamicStates[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    const VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color;
    rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.pNext = &rendering;
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &input;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &samples;
    pipeline.pDepthStencilState = &depth;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = sceneLayout_;
    for (size_t i = 0; i < scenePipelines_.size(); ++i) {
        assembly.topology = i == 2 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        raster.cullMode = i == 2 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        raster.frontFace = i == 1 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        depth.depthCompareOp = i == 2 ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;
        depth.depthWriteEnable = i == 2 ? VK_FALSE : VK_TRUE;
        vkCheck(vkCreateGraphicsPipelines(context_.device, {}, 1, &pipeline, nullptr, &scenePipelines_[i]),
                "Scene pipeline");
    }
}
void VulkanRenderer::recordScene(VkCommandBuffer cmd, const RenderView& view) {
    for (auto& group : instanceGroups_)
        group.clear();
    for (const auto& item : view.items) {
        const size_t group =
            (item.primitive == Primitive::Cube ? 0u : 2u) + (glm::determinant(glm::mat3(item.world)) < 0 ? 1u : 0u);
        if (!item.wireOnly)
            instanceGroups_[group].push_back({item.world, item.color});
        if (item.selected) {
            const auto scale = item.primitive == Primitive::Cube ? glm::vec3(1.012f) : glm::vec3(1.012f, 0, 1.012f);
            instanceGroups_[5].push_back({glm::scale(item.world, scale), glm::vec4(1.0f, .73f, .24f, 1)});
        }
    }
    if (view.grid)
        instanceGroups_[4].push_back({glm::mat4(1), glm::vec4(1)});
    size_t count{};
    for (const auto& group : instanceGroups_)
        count += group.size();
    if (!count)
        return;
    auto& frame = frames_[frameIndex_];
    if (count > frame.instanceCapacity) {
        if (frame.instances)
            vmaDestroyBuffer(allocator_, frame.instances, frame.instanceMemory);
        frame.instances = {};
        frame.instanceMemory = {};
        frame.instanceCapacity = std::max<size_t>(count * 2, 64);
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = frame.instanceCapacity * sizeof(SceneInstance);
        buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO;
        memory.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        vkCheck(vmaCreateBuffer(allocator_, &buffer, &memory, &frame.instances, &frame.instanceMemory, &info),
                "Scene instance buffer");
        frame.instanceData = info.pMappedData;
    }
    size_t offset{};
    for (const auto& group : instanceGroups_) {
        if (!group.empty())
            std::memcpy(static_cast<SceneInstance*>(frame.instanceData) + offset, group.data(),
                        group.size() * sizeof(SceneInstance));
        offset += group.size();
    }
    vkCheck(vmaFlushAllocation(allocator_, frame.instanceMemory, 0, count * sizeof(SceneInstance)),
            "Flush scene instances");
    const VkBuffer vertexBuffers[]{geometry_, frame.instances};
    const VkDeviceSize offsets[]{0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, vertexBuffers, offsets);
    vkCmdBindIndexBuffer(cmd, geometry_, indexOffset_, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cmd, sceneLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &view.viewProjection);
    uint32_t first{};
    for (size_t group = 0; group < instanceGroups_.size(); ++group) {
        const auto instances = static_cast<uint32_t>(instanceGroups_[group].size());
        if (!instances)
            continue;
        const auto& mesh = meshRanges_[group < 4 ? group / 2 : group - 2];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipelines_[group < 4 ? group % 2 : 2]);
        vkCmdDrawIndexed(cmd, mesh.indexCount, instances, mesh.firstIndex, 0, first);
        first += instances;
    }
}
} // namespace proto
