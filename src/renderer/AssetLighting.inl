// Included by AssetRenderer.cpp after the shared asset resource implementation.
// All commands use the renderer's single graphics/compute queue.
namespace asset_lighting_detail {
constexpr uint32_t tileStride = 66;
struct LightingImage {
    VkDevice device{};
    VmaAllocator allocator{};
    VkImage image{};
    VmaAllocation memory{};
    VkImageView view{}, cube{};
    std::vector<VkImageView> layers;
    uint32_t width{}, height{}, layerCount{};
    VkFormat format{};
    uint64_t bytes{};
    ~LightingImage() {
        for (auto v : layers)
            vkDestroyImageView(device, v, nullptr);
        if (cube)
            vkDestroyImageView(device, cube, nullptr);
        if (view)
            vkDestroyImageView(device, view, nullptr);
        if (image)
            vmaDestroyImage(allocator, image, memory);
    }
};
std::shared_ptr<LightingImage> lightingImage(const VulkanContext& c, VmaAllocator allocator, uint32_t width,
                                             uint32_t height, uint32_t layers, VkFormat format, VkImageUsageFlags usage,
                                             bool cube = false) {
    if (asset_gpu_detail::allocationFailureForTesting.load())
        throw std::runtime_error("Injected AssetRenderer allocation failure");
    auto image = std::make_shared<LightingImage>();
    image->device = c.device;
    image->allocator = allocator;
    image->width = width;
    image->height = height;
    image->layerCount = layers;
    image->format = format;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VmaAllocationInfo allocated{};
    vkCheck(vmaCreateImage(allocator, &info, &allocation, &image->image, &image->memory, &allocated), "Lighting image");
    image->bytes = allocated.size;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image->image;
    view.format = format;
    view.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    view.subresourceRange = {format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0,
                             1, 0, layers};
    vkCheck(vkCreateImageView(c.device, &view, nullptr, &image->view), "Lighting image view");
    if (cube) {
        view.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        vkCheck(vkCreateImageView(c.device, &view, nullptr, &image->cube), "Shadow cube-array view");
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.subresourceRange.layerCount = 1;
        image->layers.resize(layers);
        for (uint32_t i = 0; i < layers; ++i) {
            view.subresourceRange.baseArrayLayer = i;
            vkCheck(vkCreateImageView(c.device, &view, nullptr, &image->layers[i]), "Shadow face view");
        }
    }
    return image;
}
void lightingBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, uint32_t layers,
                     VkImageLayout oldLayout, VkImageLayout newLayout, VkPipelineStageFlags2 source,
                     VkAccessFlags2 read, VkPipelineStageFlags2 target, VkAccessFlags2 write) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = source;
    b.srcAccessMask = read;
    b.dstStageMask = target;
    b.dstAccessMask = write;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.image = image;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = {aspect, 0, 1, 0, layers};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dependency);
}
void lightingBufferBarrier(VkCommandBuffer cmd, VkBuffer buffer, VkPipelineStageFlags2 source, VkAccessFlags2 read,
                           VkPipelineStageFlags2 target, VkAccessFlags2 write) {
    VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    b.srcStageMask = source;
    b.srcAccessMask = read;
    b.dstStageMask = target;
    b.dstAccessMask = write;
    b.buffer = buffer;
    b.offset = 0;
    b.size = VK_WHOLE_SIZE;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dependency);
}
struct LightingUniform {
    glm::mat4 vp;
    glm::vec4 eye;
    glm::mat4 inverseVP, view;
    glm::vec4 camera;
    glm::uvec4 extent;
    glm::vec4 settings, options;
};
struct LightUniform {
    glm::vec4 positionRadius, colorIntensity, directionType, shadow;
    std::array<glm::mat4, 4> cascades;
    glm::vec4 splits;
};
struct LightingPush {
    glm::mat4 lightVP{1};
    glm::vec4 positionRadius{};
    glm::uvec4 mode{};
};
static_assert(sizeof(LightingUniform) == 272 && sizeof(LightUniform) == 336 && sizeof(LightingPush) == 96);
struct ShadowHash {
    uint64_t value{14695981039346656037ull};
    void bytes(const void* data, size_t size) {
        auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) {
            value ^= p[i];
            value *= 1099511628211ull;
        }
    }
    template <class T> void add(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes(&v, sizeof(v));
    }
    void string(const std::string& v) { bytes(v.data(), v.size()); }
};
} // namespace asset_lighting_detail
using namespace asset_lighting_detail;

struct AssetRenderer::State::Lighting {
    State& owner;
    VkPhysicalDeviceProperties deviceProperties{};
    VkDescriptorSetLayout globalLayout{}, toneLayout{};
    VkPipelineLayout layout{}, tonePipelineLayout{};
    VkDescriptorPool descriptors{};
    VkSampler sampler{};
    std::array<VkPipeline, 4> depthPipelines{}, shadowPipelines{}, basePipelines{}, lightPipelines{};
    std::array<VkPipeline, 4> wideBasePipelines{}, wideLightPipelines{};
    VkPipeline tilePipeline{}, tonePipeline{};
    struct Slot {
        EntityId id;
        uint64_t key{}, lastUse{};
        bool valid{};
    };
    struct Pool {
        std::shared_ptr<LightingImage> image;
        std::vector<Slot> slots;
        uint32_t requestedSlots{};
        uint64_t allocationBudget{};
        bool initialized{};
    };
    // M5 keeps one shared pool. Explicit package graphics use independent
    // directional CSM and point-light cube pools so their configured quality
    // and budget cannot be silently collapsed into one atlas.
    std::shared_ptr<Pool> pool;
    std::shared_ptr<Pool> sunPool;
    std::shared_ptr<Pool> pointPool;
    uint64_t generation{};
    struct Frame {
        std::shared_ptr<Buffer> view, lights, instances, indices, tiles, counters;
        std::shared_ptr<LightingImage> hdr;
        std::shared_ptr<Pool> retainedPool;
        std::shared_ptr<Pool> retainedSunPool;
        std::shared_ptr<Pool> retainedPointPool;
        VkDescriptorSet global{}, tone{};
        bool countersWritten{};
    };
    std::array<Frame, 2> frames;
    struct Surface {
        std::shared_ptr<Mesh> mesh;
        std::shared_ptr<Material> material;
        uint32_t part{}, instance{}, winding{};
        Bounds bounds;
        EntityId entity;
        glm::mat4 world;
    };
    struct Draw {
        std::shared_ptr<Mesh> mesh;
        std::shared_ptr<Material> material;
        MeshPart part;
        uint32_t winding{}, first{}, count{};
    };
    struct ShadowPass {
        glm::mat4 matrix;
        glm::vec4 positionRadius{};
        uint32_t layer{}, kind{};
        bool directional{};
        std::vector<Draw> draws;
    };
    struct Batch {
        uint32_t first{}, count{};
        std::vector<ShadowPass> shadows;
    };
    std::vector<Instance> instances;
    std::vector<uint32_t> indices;
    std::vector<Surface> visible, casters;
    std::vector<LightUniform> lights;
    std::vector<Batch> batches;

    explicit Lighting(State& state) : owner(state) {
        vkGetPhysicalDeviceProperties(owner.context.physicalDevice, &deviceProperties);
        create();
    }
    ~Lighting() {
        const auto d = owner.context.device;
        for (auto& f : frames)
            f = {};
        pool.reset();
        sunPool.reset();
        pointPool.reset();
        for (auto collection :
             {depthPipelines, shadowPipelines, basePipelines, lightPipelines, wideBasePipelines, wideLightPipelines})
            for (auto pipeline : collection)
                if (pipeline)
                    vkDestroyPipeline(d, pipeline, nullptr);
        if (tilePipeline)
            vkDestroyPipeline(d, tilePipeline, nullptr);
        if (tonePipeline)
            vkDestroyPipeline(d, tonePipeline, nullptr);
        if (layout)
            vkDestroyPipelineLayout(d, layout, nullptr);
        if (tonePipelineLayout)
            vkDestroyPipelineLayout(d, tonePipelineLayout, nullptr);
        if (descriptors)
            vkDestroyDescriptorPool(d, descriptors, nullptr);
        if (globalLayout)
            vkDestroyDescriptorSetLayout(d, globalLayout, nullptr);
        if (toneLayout)
            vkDestroyDescriptorSetLayout(d, toneLayout, nullptr);
        if (sampler)
            vkDestroySampler(d, sampler, nullptr);
    }

    void abortFrame(size_t frame) noexcept {
        if (frame >= frames.size())
            return;
        // The failed command buffer never reached the queue. Drop all
        // CPU-side planning and per-frame GPU state that it advanced; shared
        // pointers retained by the other in-flight frame keep resources used
        // by an already submitted frame alive.
        instances.clear();
        indices.clear();
        visible.clear();
        casters.clear();
        lights.clear();
        batches.clear();
        auto& current = frames[frame];
        const auto global = current.global;
        const auto tone = current.tone;
        current = {};
        current.global = global;
        current.tone = tone;
        pool.reset();
        sunPool.reset();
        pointPool.reset();
        generation = 0;
        owner.stats = {};
    }

    VkPipeline graphics(VkShaderModule vertex, VkShaderModule fragment, VkPipelineLayout pipelineLayout, VkFormat color,
                        bool writeDepth, bool equalDepth, uint32_t winding, bool shadow, bool additive,
                        bool fullscreen = false) {
        const auto d = owner.context.device;
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (auto& s : stages) {
            s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            s.pName = "main";
        }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        const VkVertexInputBindingDescription binding{0, sizeof(AssetVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attributes[]{
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(AssetVertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(AssetVertex, normal)},
            {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(AssetVertex, tangent)},
            {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(AssetVertex, color)},
            {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(AssetVertex, uv0)},
            {5, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(AssetVertex, uv1)}};
        const VkVertexInputAttributeDescription depthAttributes[]{attributes[0], attributes[3], attributes[4],
                                                                  attributes[5]};
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (!fullscreen) {
            input.vertexBindingDescriptionCount = 1;
            input.pVertexBindingDescriptions = &binding;
            input.vertexAttributeDescriptionCount = color == VK_FORMAT_UNDEFINED ? 4 : 6;
            input.pVertexAttributeDescriptions = color == VK_FORMAT_UNDEFINED ? depthAttributes : attributes;
        }
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.lineWidth = 1;
        raster.cullMode = fullscreen || winding >= 2 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        raster.frontFace = ((winding % 2) == 0) ^ shadow ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = !fullscreen;
        depth.depthWriteEnable = writeDepth;
        depth.depthCompareOp = equalDepth ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = 15;
        attachment.blendEnable = additive;
        attachment.srcColorBlendFactor = attachment.dstColorBlendFactor = attachment.srcAlphaBlendFactor =
            attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.colorBlendOp = attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = color == VK_FORMAT_UNDEFINED ? 0 : 1;
        blend.pAttachments = &attachment;
        const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = color == VK_FORMAT_UNDEFINED ? 0 : 1;
        rendering.pColorAttachmentFormats = &color;
        rendering.depthAttachmentFormat = fullscreen ? VK_FORMAT_UNDEFINED : VK_FORMAT_D32_SFLOAT;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &samples;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = pipelineLayout;
        VkPipeline pipeline{};
        vkCheck(vkCreateGraphicsPipelines(d, {}, 1, &info, nullptr, &pipeline), "Lighting graphics pipeline");
        return pipeline;
    }
    void create() {
        const auto d = owner.context.device;
        std::array<VkDescriptorSetLayoutBinding, 11> bindings{};
        for (uint32_t i = 0; i < 11; ++i) {
            auto& b = bindings[i];
            b.binding = i;
            b.descriptorCount = 1;
            b.descriptorType = i == 0                 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                               : (i >= 5 && i <= 9)   ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                      : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo descriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        descriptor.bindingCount = 11;
        descriptor.pBindings = bindings.data();
        vkCheck(vkCreateDescriptorSetLayout(d, &descriptor, nullptr, &globalLayout), "Lighting descriptor layout");
        const VkDescriptorSetLayout layouts[]{globalLayout, owner.materialLayout};
        VkPushConstantRange push{VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(LightingPush)};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 2;
        layoutInfo.pSetLayouts = layouts;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &push;
        vkCheck(vkCreatePipelineLayout(d, &layoutInfo, nullptr, &layout), "Lighting pipeline layout");
        VkDescriptorSetLayoutBinding toneBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                 VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        descriptor.bindingCount = 1;
        descriptor.pBindings = &toneBinding;
        vkCheck(vkCreateDescriptorSetLayout(d, &descriptor, nullptr, &toneLayout), "Tone descriptor layout");
        push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &toneLayout;
        vkCheck(vkCreatePipelineLayout(d, &layoutInfo, nullptr, &tonePipelineLayout), "Tone pipeline layout");
        const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2},
                                           {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 12},
                                           {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 4;
        poolInfo.poolSizeCount = 3;
        poolInfo.pPoolSizes = sizes;
        vkCheck(vkCreateDescriptorPool(d, &poolInfo, nullptr, &descriptors), "Lighting descriptor pool");
        for (auto& f : frames) {
            VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            a.descriptorPool = descriptors;
            a.descriptorSetCount = 1;
            a.pSetLayouts = &globalLayout;
            vkCheck(vkAllocateDescriptorSets(d, &a, &f.global), "Lighting descriptors");
            a.pSetLayouts = &toneLayout;
            vkCheck(vkAllocateDescriptorSets(d, &a, &f.tone), "Tone descriptors");
        }
        VkSamplerCreateInfo sampling{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampling.minFilter = sampling.magFilter = VK_FILTER_NEAREST;
        sampling.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampling.addressModeU = sampling.addressModeV = sampling.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCheck(vkCreateSampler(d, &sampling, nullptr, &sampler), "Lighting nearest sampler");
        std::vector<VkShaderModule> shaders;
        const auto load = [&](const char* file) {
            auto m = shader(d, owner.shaderDirectory / file);
            shaders.push_back(m);
            return m;
        };
        struct Cleanup {
            VkDevice d;
            std::vector<VkShaderModule>& v;
            ~Cleanup() {
                for (auto m : v)
                    vkDestroyShaderModule(d, m, nullptr);
            }
        } cleanup{d, shaders};
        const auto vertex = load("lit.vert.spv"), depthVertex = load("shadow.vert.spv"),
                   cameraVertex = load("camera_depth.vert.spv"), cameraFragment = load("camera_depth.frag.spv"),
                   fragment = load("lit.frag.spv"), shadow = load("shadow.frag.spv"),
                   toneVertex = load("tone.vert.spv"), toneFragment = load("tone.frag.spv"),
                   compute = load("light_tiles.comp.spv");
        for (uint32_t i = 0; i < 4; ++i) {
            depthPipelines[i] =
                graphics(cameraVertex, cameraFragment, layout, VK_FORMAT_UNDEFINED, true, false, i, false, false);
            shadowPipelines[i] =
                graphics(depthVertex, shadow, layout, VK_FORMAT_UNDEFINED, true, false, i, true, false);
            basePipelines[i] =
                graphics(vertex, fragment, layout, VK_FORMAT_R16G16B16A16_SFLOAT, false, true, i, false, false);
            lightPipelines[i] =
                graphics(vertex, fragment, layout, VK_FORMAT_R16G16B16A16_SFLOAT, false, true, i, false, true);
            wideBasePipelines[i] =
                graphics(vertex, fragment, layout, VK_FORMAT_R32G32B32A32_SFLOAT, false, true, i, false, false);
            wideLightPipelines[i] =
                graphics(vertex, fragment, layout, VK_FORMAT_R32G32B32A32_SFLOAT, false, true, i, false, true);
        }
        tonePipeline = graphics(toneVertex, toneFragment, tonePipelineLayout, VK_FORMAT_R8G8B8A8_UNORM, false, false, 2,
                                false, false, true);
        VkComputePipelineCreateInfo computeInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        computeInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        computeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        computeInfo.stage.module = compute;
        computeInfo.stage.pName = "main";
        computeInfo.layout = layout;
        vkCheck(vkCreateComputePipelines(d, {}, 1, &computeInfo, nullptr, &tilePipeline), "Tile culling pipeline");
    }
    std::shared_ptr<Pool>& poolFor(bool directional) { return directional ? sunPool : pointPool; }
    const std::shared_ptr<Pool>& poolFor(bool directional) const { return directional ? sunPool : pointPool; }
    void ensurePool(const RenderView& view, size_t shadowLights, bool directional) {
        if (!view.runtimeLighting.explicitSettings) {
            const uint32_t resolution = shadowLights ? view.lighting.shadowResolution : 1;
            const uint64_t budget = uint64_t(view.lighting.shadowPoolMiB) * 1024 * 1024,
                           slotBytes = uint64_t(resolution) * resolution * 4 * 6;
            if (!shadowLights && pool && pool->image->width == view.lighting.shadowResolution &&
                pool->image->bytes <= budget)
                return;
            uint32_t count = shadowLights
                                 ? static_cast<uint32_t>(std::min<uint64_t>(
                                       {budget / slotBytes, deviceProperties.limits.maxImageArrayLayers / 6,
                                        shadowLights}))
                                 : 1;
            if (!count)
                throw std::runtime_error("Shadow budget cannot fit one cube-array slot");
            if (pool && pool->image->width == resolution && pool->image->bytes <= budget &&
                (pool->slots.size() >= count ||
                 (pool->allocationBudget == budget && pool->requestedSlots >= count)))
                return;
            const auto requestedSlots = count;
            for (;;) {
                auto image = lightingImage(owner.context, owner.allocator, resolution, resolution, count * 6,
                                           VK_FORMAT_D32_SFLOAT,
                                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                           true);
                if (image->bytes <= budget || !shadowLights) {
                    auto next = std::make_shared<Pool>();
                    next->image = std::move(image);
                    next->slots.resize(count);
                    next->requestedSlots = requestedSlots;
                    next->allocationBudget = budget;
                    pool = std::move(next);
                    return;
                }
                if (count == 1)
                    throw std::runtime_error("Shadow budget is smaller than the GPU allocation requirement");
                --count;
            }
        }
        const auto& atlas = directional ? view.runtimeLighting.sun : view.runtimeLighting.point;
        const uint32_t resolution = atlas.resolution;
        const uint64_t slotBytes = uint64_t(resolution) * resolution * 4 * 6;
        const uint64_t budget = directional ? UINT64_MAX : uint64_t(atlas.poolBudgetMiB) * 1024 * 1024;
        auto& target = poolFor(directional);
        uint32_t count = shadowLights
                             ? static_cast<uint32_t>(std::min<uint64_t>(
                                   {budget / slotBytes, deviceProperties.limits.maxImageArrayLayers / 6,
                                    shadowLights}))
                             : 1;
        if (!count)
            throw std::runtime_error("Shadow budget cannot fit one cube-array slot");
        if (target && target->image->width == resolution && target->image->bytes <= budget &&
            (target->slots.size() >= count ||
             (target->allocationBudget == budget && target->requestedSlots >= count)))
            return;
        const auto requestedSlots = count;
        for (;;) {
            auto image = lightingImage(owner.context, owner.allocator, resolution, resolution, count * 6,
                                       VK_FORMAT_D32_SFLOAT,
                                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                       true);
            if (image->bytes <= budget || directional) {
                auto next = std::make_shared<Pool>();
                next->image = std::move(image);
                next->slots.resize(count);
                next->requestedSlots = requestedSlots;
                next->allocationBudget = budget;
                target = std::move(next);
                return;
            }
            if (count == 1)
                throw std::runtime_error("Point shadow budget is smaller than one cube-array slot");
            --count;
        }
    }
    std::vector<Draw> draws(const std::vector<Surface>& surfaces, const std::function<bool(const Surface&)>& include) {
        struct Group {
            Draw draw;
            std::vector<uint32_t> instances;
        };
        std::map<std::tuple<AssetId, uint32_t, AssetId, uint32_t>, Group> groups;
        for (const auto& s : surfaces) {
            if (!include(s))
                continue;
            auto& group = groups[{s.mesh->source->id, s.part, s.material->source->id, s.winding}];
            group.draw = {s.mesh, s.material, s.mesh->source->parts[s.part], s.winding, 0, 0};
            group.instances.push_back(s.instance);
        }
        std::vector<Draw> result;
        for (auto& [key, g] : groups) {
            g.draw.first = static_cast<uint32_t>(indices.size());
            g.draw.count = static_cast<uint32_t>(g.instances.size());
            indices.insert(indices.end(), g.instances.begin(), g.instances.end());
            result.push_back(std::move(g.draw));
        }
        return result;
    }
    void gather(const RenderView& view) {
        owner.stats.rawInstances = std::max(view.imported.size(), view.casters.size());
        owner.stats.visibleInstances = view.imported.size();
        owner.stats.culledInstances = owner.stats.rawInstances >= owner.stats.visibleInstances
                                          ? owner.stats.rawInstances - owner.stats.visibleInstances
                                          : 0;
        instances.clear();
        indices.clear();
        visible.clear();
        casters.clear();
        lights.clear();
        batches.clear();
        std::unordered_map<EntityId, uint32_t> lookup;
        lookup.reserve(std::max(view.imported.size(), view.casters.size()));
        // Resolution can inspect source revisions and texture readiness. Do it
        // once per material in this gather, so the next frame still observes
        // edits, completed uploads and texture-quality changes immediately.
        std::unordered_map<AssetId, std::shared_ptr<Material>> frameMaterials;
        const auto materialFor = [&](AssetId id) {
            const auto [entry, inserted] = frameMaterials.try_emplace(id);
            if (inserted)
                entry->second = owner.material(id);
            return entry->second;
        };
        const auto append = [&](const ImportedItem& item, std::vector<Surface>& target) {
            auto found = owner.meshes.find(item.mesh);
            if (found == owner.meshes.end())
                return;
            auto mesh = found->second;
            if (!mesh->ready)
                mesh = mesh->previous;
            if (!mesh || !mesh->ready)
                return;
            uint32_t index{};
            if (auto existing = lookup.find(item.id); item.id && existing != lookup.end())
                index = existing->second;
            else {
                index = static_cast<uint32_t>(instances.size());
                const float sign = glm::determinant(glm::mat3(item.world)) < 0 ? -1.0f : 1.0f;
                instances.push_back({item.world,
                                     {item.normal[0], sign},
                                     {item.normal[1], float(item.receiveShadows)},
                                     {item.normal[2], 0},
                                     item.color});
                if (item.id)
                    lookup[item.id] = index;
            }
            const auto bounds = transformBounds(mesh->source->bounds, item.world);
            for (uint32_t part = 0; part < mesh->source->parts.size(); ++part) {
                auto material = materialFor(item.materials.empty() ? mesh->source->parts[part].material
                                                                   : item.materials.at(part));
                if (!material)
                    continue;
                const uint32_t winding =
                    (material->source->values.doubleSided ? 2u : 0u) + (instances[index].normal0.w < 0 ? 1u : 0u);
                target.push_back({mesh, material, part, index, winding, bounds, item.id, item.world});
            }
        };
        for (const auto& item : view.imported)
            append(item, visible);
        for (const auto& item : view.casters)
            append(item, casters);
    }
    uint64_t shadowKey(const SceneLight& light, const LightUniform& uniform) {
        ShadowHash hash;
        hash.add(light.directional);
        if (!light.directional) {
            hash.add(light.position);
            hash.add(light.radius);
        }
        if (light.directional) {
            hash.add(light.direction);
            hash.add(uniform.cascades);
            hash.add(uniform.splits);
        }
        std::array<ShadowFrustum, 4> frusta;
        if (light.directional)
            for (uint32_t i = 0; i < uint32_t(uniform.shadow.y); ++i)
                frusta[i] = makeShadowFrustum(uniform.cascades[i]);
        std::vector<const Surface*> relevant;
        relevant.reserve(casters.size());
        for (const auto& surface : casters)
            if (light.directional
                    ? std::any_of(frusta.begin(), frusta.begin() + uint32_t(uniform.shadow.y),
                                  [&](const auto& frustum) { return shadowFrustumContains(surface.bounds, frustum); })
                    : sphereTouches(surface.bounds, light.position, light.radius))
                relevant.push_back(&surface);
        std::sort(relevant.begin(), relevant.end(),
                  [](auto a, auto b) { return std::tie(a->entity, a->part) < std::tie(b->entity, b->part); });
        for (const auto* s : relevant) {
            hash.add(s->entity);
            hash.add(s->world);
            hash.add(s->part);
            hash.add(s->mesh->source->id);
            hash.string(s->mesh->source->revision);
            const auto& m = *s->material->source;
            hash.add(m.values.doubleSided);
            hash.add(m.values.mask);
            if (m.values.mask) {
                hash.add(instances[s->instance].color.a);
                hash.add(s->material->alphaSampler);
                hash.add(m.values.baseColor.a);
                hash.add(m.values.alphaCutoff);
                const auto& slot = m.textures[0];
                hash.add(s->material->images[0]->baseMip);
                hash.add(slot.offset);
                hash.add(slot.scale);
                hash.add(slot.rotation);
                hash.add(slot.texCoord);
                if (slot.texture)
                    hash.string(s->material->images[0]->source->hash);
            }
        }
        return hash.value;
    }
    void plan(const RenderView& view, float aspect) {
        std::vector<SceneLight> active;
        const auto cameraFrustum = makeShadowFrustum(view.viewProjection);
        for (const auto& light : view.lights) {
            if (light.intensity <= 0)
                continue;
            if (!light.directional) {
                const Bounds area{light.position - glm::vec3(light.radius), light.position + glm::vec3(light.radius)};
                if (!shadowFrustumContains(area, cameraFrustum))
                    continue;
            }
            active.push_back(light);
        }
        std::sort(active.begin(), active.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        const auto shadowEnabled = [&](const SceneLight& light) {
            if (!view.lighting.shadows || !light.shadows)
                return false;
            if (!view.runtimeLighting.explicitSettings)
                return true;
            return light.directional ? view.runtimeLighting.sun.enabled : view.runtimeLighting.point.enabled;
        };
        const size_t shadowCount = std::count_if(active.begin(), active.end(), shadowEnabled);
        const size_t sunShadowCount =
            std::count_if(active.begin(), active.end(), [&](const auto& l) { return l.directional && shadowEnabled(l); });
        const size_t pointShadowCount =
            std::count_if(active.begin(), active.end(), [&](const auto& l) { return !l.directional && shadowEnabled(l); });
        if (view.runtimeLighting.explicitSettings) {
            ensurePool(view, sunShadowCount, true);
            ensurePool(view, pointShadowCount, false);
        } else {
            ensurePool(view, shadowCount, false);
        }
        ++generation;
        owner.stats.lights = static_cast<uint32_t>(active.size());
        if (view.runtimeLighting.explicitSettings) {
            owner.stats.shadowSlots = static_cast<uint32_t>((sunShadowCount ? sunPool->slots.size() : 0) +
                                                            (pointShadowCount ? pointPool->slots.size() : 0));
            owner.stats.sunShadowBytes = sunPool->image->bytes;
            owner.stats.pointShadowBytes = pointPool->image->bytes;
            owner.stats.shadowBytes = owner.stats.sunShadowBytes + owner.stats.pointShadowBytes;
        } else {
            owner.stats.shadowSlots = shadowCount ? static_cast<uint32_t>(pool->slots.size()) : 0;
            owner.stats.shadowBytes = pool->image->bytes;
            owner.stats.sunShadowBytes = 0;
            owner.stats.pointShadowBytes = owner.stats.shadowBytes;
        }
        std::vector<Bounds> bounds;
        bounds.reserve(casters.size());
        for (const auto& s : casters)
            bounds.push_back(s.bounds);
        for (const auto& light : active) {
            LightUniform uniform{};
            uniform.positionRadius = {light.position, light.radius};
            uniform.colorIntensity = {light.color, light.intensity};
            uniform.directionType = {light.direction, light.directional ? 0.0f : 1.0f};
            const uint32_t taps = view.runtimeLighting.explicitSettings
                                      ? (light.directional ? view.runtimeLighting.sun.filterTaps
                                                           : view.runtimeLighting.point.filterTaps)
                                      : (view.lighting.filteredShadows ? 4u : 1u);
            uniform.shadow = {-1, float(view.lighting.shadowCascades), view.lighting.shadowDistance, float(taps)};
            if (light.directional) {
                auto cascades = directionalCascades(view, light.direction, aspect, bounds);
                uniform.cascades = cascades.matrices;
                uniform.splits = cascades.splits;
            }
            lights.push_back(uniform);
        }
        uint32_t begin{};
        while (begin < active.size()) {
            uint32_t end = begin, shadowed{}, sunShadowed{}, pointShadowed{};
            while (end < active.size()) {
                if (shadowEnabled(active[end])) {
                    if (view.runtimeLighting.explicitSettings) {
                        auto& current = *poolFor(active[end].directional);
                        if (active[end].directional ? sunShadowed == current.slots.size()
                                                     : pointShadowed == current.slots.size())
                            break;
                        if (active[end].directional)
                            ++sunShadowed;
                        else
                            ++pointShadowed;
                    } else {
                        if (shadowed == pool->slots.size())
                            break;
                        ++shadowed;
                    }
                }
                ++end;
            }
            Batch batch{begin, end - begin, {}};
            std::set<EntityId> reserved;
            for (uint32_t i = begin; i < end; ++i)
                if (shadowEnabled(active[i]))
                    reserved.insert(active[i].id);
            for (uint32_t i = begin; i < end; ++i) {
                const auto& light = active[i];
                if (!shadowEnabled(light))
                    continue;
                auto& current = view.runtimeLighting.explicitSettings ? *poolFor(light.directional) : *pool;
                auto chosen = current.slots.size();
                for (size_t slot = 0; slot < current.slots.size(); ++slot)
                    if (current.slots[slot].id == light.id) {
                        chosen = slot;
                        break;
                    }
                if (chosen == current.slots.size()) {
                    uint64_t oldest = UINT64_MAX;
                    for (size_t slot = 0; slot < current.slots.size(); ++slot)
                        if (!reserved.contains(current.slots[slot].id) && current.slots[slot].lastUse < oldest) {
                            oldest = current.slots[slot].lastUse;
                            chosen = slot;
                        }
                }
                if (chosen == current.slots.size())
                    throw std::runtime_error("Shadow batch slot scheduling failed");
                auto& slot = current.slots[chosen];
                auto& uniform = lights[i];
                uniform.shadow.x = float(chosen);
                const auto key = shadowKey(light, uniform);
                const bool update =
                    view.lighting.forceShadowRefresh || !slot.valid || slot.id != light.id || slot.key != key;
                slot = {light.id, key, generation, true};
                if (!update) {
                    ++owner.stats.shadowCacheHits;
                    continue;
                }
                ++owner.stats.shadowMisses;
                const auto points =
                    light.directional ? std::array<glm::mat4, 6>{} : pointShadowMatrices(light.position, light.radius);
                const uint32_t faces = light.directional ? view.lighting.shadowCascades : 6;
                for (uint32_t face = 0; face < faces; ++face) {
                    ShadowPass pass;
                    pass.kind = light.directional ? 1u : 2u;
                    pass.directional = light.directional;
                    pass.layer = static_cast<uint32_t>(chosen) * 6 + face;
                    pass.matrix = light.directional ? uniform.cascades[face] : points[face];
                    pass.positionRadius = {light.position, light.radius};
                    const auto faceFrustum = makeShadowFrustum(pass.matrix);
                    pass.draws = draws(casters, [&](const auto& s) {
                        return (light.directional || sphereTouches(s.bounds, light.position, light.radius)) &&
                               shadowFrustumContains(s.bounds, faceFrustum);
                    });
                    batch.shadows.push_back(std::move(pass));
                    ++owner.stats.shadowFaces;
                }
            }
            batches.push_back(std::move(batch));
            begin = end;
        }
        owner.stats.batches = static_cast<uint32_t>(batches.size());
    }
    void host(std::shared_ptr<Buffer>& target, size_t bytes, VkBufferUsageFlags usage, const void* data = nullptr) {
        bytes = std::max<size_t>(bytes, 16);
        if (!target || target->size < bytes)
            target = buffer(owner.context.device, owner.allocator, bytes, usage, true);
        if (data) {
            std::memcpy(target->mapped, data, bytes);
            vkCheck(vmaFlushAllocation(owner.allocator, target->memory, 0, bytes), "Lighting buffer flush");
        }
    }
    void uploadFrame(Frame& f, const RenderView& view, VkExtent2D extent, VkImageView depth) {
        const auto tilesX = (extent.width + 15) / 16, tilesY = (extent.height + 15) / 16;
        const float tangent = std::tan(glm::radians(view.verticalFovDegrees) * .5f);
        LightingUniform uniform{
            view.viewProjection,
            glm::vec4(view.eye, 1),
            glm::inverse(view.viewProjection),
            view.cameraView,
            {tangent * float(extent.width) / float(extent.height), tangent, view.cameraNear, view.cameraFar},
            {extent.width, extent.height, tilesX, tileStride},
            {view.lighting.ambient, view.lighting.depthBias, view.lighting.normalBias,
             float(view.lighting.filteredShadows)},
            {view.exposure, float(view.lighting.referenceLighting), 0, 0}};
        host(f.view, sizeof(uniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &uniform);
        host(f.lights, std::max<size_t>(1, lights.size()) * sizeof(LightUniform), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!lights.empty()) {
            std::memcpy(f.lights->mapped, lights.data(), lights.size() * sizeof(LightUniform));
            vkCheck(vmaFlushAllocation(owner.allocator, f.lights->memory, 0, VK_WHOLE_SIZE), "Light data flush");
        }
        host(f.instances, std::max<size_t>(1, instances.size()) * sizeof(Instance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!instances.empty()) {
            std::memcpy(f.instances->mapped, instances.data(), instances.size() * sizeof(Instance));
            vkCheck(vmaFlushAllocation(owner.allocator, f.instances->memory, 0, VK_WHOLE_SIZE), "Instance data flush");
        }
        host(f.indices, std::max<size_t>(4, indices.size()) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!indices.empty()) {
            std::memcpy(f.indices->mapped, indices.data(), indices.size() * 4);
            vkCheck(vmaFlushAllocation(owner.allocator, f.indices->memory, 0, VK_WHOLE_SIZE), "Draw index flush");
        }
        host(f.counters, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        owner.stats.dynamicLightingUploadBytes = sizeof(uniform) + lights.size() * sizeof(LightUniform) +
                                                 instances.size() * sizeof(Instance) + indices.size() * sizeof(uint32_t);
        const size_t tileBytes = size_t(tilesX) * tilesY * tileStride * 4;
        if (!f.tiles || f.tiles->size < tileBytes)
            f.tiles =
                buffer(owner.context.device, owner.allocator, tileBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        const bool sameSize = f.hdr && f.hdr->width == extent.width && f.hdr->height == extent.height;
        // Hundreds of half-float blends can round away individually dim lights.
        // Promote batched frames to float32; retain promotion at this size so a
        // light crossing the camera edge does not churn allocations each frame.
        const bool wide = batches.size() > 1 || (sameSize && f.hdr->format == VK_FORMAT_R32G32B32A32_SFLOAT);
        const auto hdrFormat = wide ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R16G16B16A16_SFLOAT;
        if (!sameSize || f.hdr->format != hdrFormat)
            f.hdr = lightingImage(owner.context, owner.allocator, extent.width, extent.height, 1, hdrFormat,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        const VkDescriptorBufferInfo bufferInfo[]{
            {f.view->buffer, 0, sizeof(uniform)},    {f.lights->buffer, 0, f.lights->size},
            {f.tiles->buffer, 0, tileBytes},         {f.instances->buffer, 0, f.instances->size},
            {f.indices->buffer, 0, f.indices->size}, {f.counters->buffer, 0, 16}};
        const auto& sun = view.runtimeLighting.explicitSettings ? sunPool : pool;
        const auto& point = view.runtimeLighting.explicitSettings ? pointPool : pool;
        const VkDescriptorImageInfo imageInfo[]{{sampler, sun->image->view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                                {sampler, sun->image->cube, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                                {sampler, point->image->view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                                {sampler, point->image->cube, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                                {sampler, depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                                                {sampler, f.hdr->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        std::array<VkWriteDescriptorSet, 12> writes{};
        for (uint32_t i = 0; i < 11; ++i) {
            auto& w = writes[i];
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = f.global;
            w.dstBinding = i;
            w.descriptorCount = 1;
            w.descriptorType = i == 0                 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                               : (i >= 5 && i <= 9)   ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                      : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (i >= 5 && i <= 9)
                w.pImageInfo = &imageInfo[i - 5];
            else
                w.pBufferInfo = &bufferInfo[i == 10 ? 5 : i];
        }
        writes[11] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[11].dstSet = f.tone;
        writes[11].descriptorCount = 1;
        writes[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[11].pImageInfo = &imageInfo[5];
        vkUpdateDescriptorSets(owner.context.device, 12, writes.data(), 0, nullptr);
        f.retainedPool = view.runtimeLighting.explicitSettings ? nullptr : pool;
        f.retainedSunPool = sun;
        f.retainedPointPool = point;
    }
    void viewport(VkCommandBuffer cmd, VkExtent2D extent, bool positive = false) {
        VkViewport viewport{0,
                            positive ? 0.0f : float(extent.height),
                            float(extent.width),
                            positive ? float(extent.height) : -float(extent.height),
                            0,
                            1};
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }
    void push(VkCommandBuffer cmd, const LightingPush& value) {
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(value),
                           &value);
    }
    enum class DrawPass { Depth, Shadow, Base, Lighting };
    void draw(VkCommandBuffer cmd, size_t frame, const std::vector<Draw>& draws,
              const std::array<VkPipeline, 4>& pipelines, DrawPass pass) {
        for (const auto& d : draws) {
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &d.mesh->data->buffer, &offset);
            vkCmdBindIndexBuffer(cmd, d.mesh->data->buffer, d.mesh->indexOffset, VK_INDEX_TYPE_UINT32);
            const VkDescriptorSet sets[]{frames[frame].global, d.material->set};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, sets, 0, nullptr);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[d.winding]);
            vkCmdDrawIndexed(cmd, d.part.indexCount, d.count, d.part.firstIndex, 0, d.first);
            owner.frames[frame].retained.push_back(d.mesh);
            owner.frames[frame].retained.push_back(d.material);
            ++owner.stats.drawCalls;
            ++owner.stats.totalDrawCalls;
            switch (pass) {
            case DrawPass::Depth:
                ++owner.stats.depthDrawCalls;
                break;
            case DrawPass::Shadow:
                ++owner.stats.shadowDrawCalls;
                break;
            case DrawPass::Base:
                ++owner.stats.baseDrawCalls;
                break;
            case DrawPass::Lighting:
                ++owner.stats.lightingDrawCalls;
                break;
            }
        }
    }
    void depthPass(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent) {
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = target;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {1, 0};
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, extent};
        rendering.layerCount = 1;
        rendering.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &rendering);
    }
    void hdrPass(VkCommandBuffer cmd, Frame& f, VkImageView depth, VkExtent2D extent, bool clear) {
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = f.hdr->view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{.005f, .008f, .012f, 1}};
        VkRenderingAttachmentInfo d{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        d.imageView = depth;
        d.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
        d.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        d.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, extent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        rendering.pDepthAttachment = &d;
        vkCmdBeginRendering(cmd, &rendering);
        viewport(cmd, extent);
    }
    void render(size_t frame, VkCommandBuffer cmd, const RenderView& view, VkExtent2D extent, VkImage color,
                VkImageView colorView, VkImage depth, VkImageView depthView) {
        (void)color;
        auto& f = frames[frame];
        owner.stats = {};
        if (f.countersWritten) {
            vkCheck(vmaInvalidateAllocation(owner.allocator, f.counters->memory, 0, 16), "Tile counter readback");
            owner.stats.tileOverflows = *static_cast<const uint32_t*>(f.counters->mapped);
        }
        owner.stats.assetUploadBytes = owner.lastAssetUploadBytes;
        const auto gatherStart = Clock::now();
        gather(view);
        owner.stats.gatherMs = milliseconds(gatherStart);
        auto main = draws(visible, [](const auto&) { return true; });
        const auto planStart = Clock::now();
        plan(view, float(extent.width) / float(extent.height));
        owner.stats.planMs = milliseconds(planStart);
        const auto uploadStart = Clock::now();
        uploadFrame(f, view, extent, depthView);
        owner.stats.uploadFrameMs = milliseconds(uploadStart);
        const auto earlyLate =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        const auto initializePool = [&](const std::shared_ptr<Pool>& current) {
            if (current->initialized)
                return;
            lightingBarrier(cmd, current->image->image, VK_IMAGE_ASPECT_DEPTH_BIT, current->image->layerCount,
                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
                            0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            const VkClearDepthStencilValue clear{1, 0};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, current->image->layerCount};
            vkCmdClearDepthStencilImage(cmd, current->image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                        &range);
            lightingBarrier(cmd, current->image->image, VK_IMAGE_ASPECT_DEPTH_BIT, current->image->layerCount,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            current->initialized = true;
        };
        initializePool(view.runtimeLighting.explicitSettings ? sunPool : pool);
        if (view.runtimeLighting.explicitSettings)
            initializePool(pointPool);
        depthPass(cmd, depthView, extent);
        viewport(cmd, extent);
        LightingPush values;
        values.mode.x = 0;
        push(cmd, values);
        draw(cmd, frame, main, depthPipelines, DrawPass::Depth);
        vkCmdEndRendering(cmd);
        lightingBarrier(cmd, depth, VK_IMAGE_ASPECT_DEPTH_BIT, 1, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, earlyLate,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | earlyLate,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
        lightingBarrier(cmd, f.hdr->image, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        hdrPass(cmd, f, depthView, extent, true);
        values.mode.x = 3;
        push(cmd, values);
        draw(cmd, frame, main, f.hdr->format == VK_FORMAT_R32G32B32A32_SFLOAT ? wideBasePipelines : basePipelines,
             DrawPass::Base);
        vkCmdEndRendering(cmd);
        lightingBufferBarrier(cmd, f.counters->buffer, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT,
                              VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(cmd, f.counters->buffer, 0, 16, 0);
        lightingBufferBarrier(cmd, f.counters->buffer, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        bool tileUsed = false;
        for (const auto& batch : batches) {
            if (!batch.shadows.empty()) {
                const bool hasSun = std::any_of(batch.shadows.begin(), batch.shadows.end(),
                                                [](const auto& shadow) { return shadow.directional; });
                const bool hasPoint = std::any_of(batch.shadows.begin(), batch.shadows.end(),
                                                  [](const auto& shadow) { return !shadow.directional; });
                const auto transitionPool = [&](const std::shared_ptr<Pool>& current, bool toAttachment) {
                    if (!current)
                        return;
                    lightingBarrier(cmd, current->image->image, VK_IMAGE_ASPECT_DEPTH_BIT, current->image->layerCount,
                                    toAttachment ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                                                 : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                    toAttachment ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
                                                 : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                                    toAttachment ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : earlyLate,
                                    toAttachment ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                                                 : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                    toAttachment ? earlyLate : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                    toAttachment ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                                 : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                };
                if (view.runtimeLighting.explicitSettings) {
                    if (hasSun)
                        transitionPool(sunPool, true);
                    if (hasPoint)
                        transitionPool(pointPool, true);
                } else if (hasSun || hasPoint) {
                    // Legacy lighting uses one shared depth atlas. Transition
                    // it once even when a batch mixes directional and point
                    // shadows; repeating the barrier would describe the same
                    // image layout transition twice.
                    transitionPool(pool, true);
                }
                for (const auto& shadow : batch.shadows) {
                    const auto& current = view.runtimeLighting.explicitSettings
                                              ? poolFor(shadow.directional)
                                              : pool;
                    const VkExtent2D size{current->image->width, current->image->height};
                    depthPass(cmd, current->image->layers[shadow.layer], size);
                    viewport(cmd, size, true);
                    values.lightVP = shadow.matrix;
                    values.positionRadius = shadow.positionRadius;
                    values.mode = {shadow.kind, 0, 0, 0};
                    push(cmd, values);
                    draw(cmd, frame, shadow.draws, shadowPipelines, DrawPass::Shadow);
                    vkCmdEndRendering(cmd);
                }
                if (view.runtimeLighting.explicitSettings) {
                    if (hasSun)
                        transitionPool(sunPool, false);
                    if (hasPoint)
                        transitionPool(pointPool, false);
                } else if (hasSun || hasPoint) {
                    transitionPool(pool, false);
                }
            }
            if (tileUsed)
                lightingBufferBarrier(cmd, f.tiles->buffer, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
            values.mode = {4, batch.first, batch.count, 0};
            push(cmd, values);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tilePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &f.global, 0, nullptr);
            vkCmdDispatch(cmd, (extent.width + 15) / 16, (extent.height + 15) / 16, 1);
            ++owner.stats.dispatchCount;
            lightingBufferBarrier(cmd, f.tiles->buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
            tileUsed = true;
            lightingBarrier(cmd, f.hdr->image, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            hdrPass(cmd, f, depthView, extent, false);
            push(cmd, values);
            draw(cmd, frame, main,
                 f.hdr->format == VK_FORMAT_R32G32B32A32_SFLOAT ? wideLightPipelines : lightPipelines,
                 DrawPass::Lighting);
            vkCmdEndRendering(cmd);
        }
        lightingBufferBarrier(cmd, f.counters->buffer,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        f.countersWritten = true;
        lightingBarrier(cmd, f.hdr->image, VK_IMAGE_ASPECT_COLOR_BIT, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        VkRenderingAttachmentInfo target{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        target.imageView = colorView;
        target.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        target.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        target.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, extent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &target;
        vkCmdBeginRendering(cmd, &rendering);
        viewport(cmd, extent, true);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonePipelineLayout, 0, 1, &f.tone, 0, nullptr);
        vkCmdPushConstants(cmd, tonePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &view.exposure);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        ++owner.stats.toneMapDrawCalls;
        ++owner.stats.totalDrawCalls;
        vkCmdEndRendering(cmd);
    }
};
void AssetRenderer::renderLit(size_t frame, VkCommandBuffer command, const RenderView& view, VkExtent2D extent,
                              VkImage color, VkImageView colorView, VkImage depth, VkImageView depthView) {
    if (!state_->lighting)
        state_->lighting = std::make_shared<State::Lighting>(*state_);
    state_->lighting->render(frame, command, view, extent, color, colorView, depth, depthView);
    if (asset_gpu_detail::allocationFailureAfterLightingForTesting.load())
        throw std::runtime_error("Injected late AssetRenderer allocation failure");
}
const LightingStats& AssetRenderer::lightingStats() const {
    return state_->stats;
}
