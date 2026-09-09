#include "renderer/AssetRenderer.hpp"
#include "renderer/LightingMath.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

namespace proto {
namespace asset_gpu_detail {
std::atomic_bool allocationFailureForTesting{false};
std::atomic_bool allocationFailureAfterUploadForTesting{false};
std::atomic_bool allocationFailureAfterLightingForTesting{false};
constexpr size_t uploadBudget = 4 * 1024 * 1024;
struct Buffer {
    VkDevice device{};
    VmaAllocator allocator{};
    VkBuffer buffer{};
    VmaAllocation memory{};
    void* mapped{};
    size_t size{};
    ~Buffer() {
        if (buffer)
            vmaDestroyBuffer(allocator, buffer, memory);
    }
};
std::shared_ptr<Buffer> buffer(VkDevice device, VmaAllocator allocator, size_t size, VkBufferUsageFlags usage,
                               bool host) {
    if (allocationFailureForTesting.load())
        throw std::runtime_error("Injected AssetRenderer allocation failure");
    auto b = std::make_shared<Buffer>();
    b->device = device;
    b->allocator = allocator;
    b->size = size;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    VmaAllocationCreateInfo memory{};
    memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (host)
        memory.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo allocation{};
    vkCheck(vmaCreateBuffer(allocator, &info, &memory, &b->buffer, &b->memory, &allocation), "Asset buffer");
    b->mapped = allocation.pMappedData;
    return b;
}
struct Image {
    VkDevice device{};
    VmaAllocator allocator{};
    VkImage image{};
    VmaAllocation memory{};
    VkImageView view{};
    std::shared_ptr<const TexturePixels> source;
    size_t baseMip{}, mip{}, row{};
    bool ready{};
    ~Image() {
        if (view)
            vkDestroyImageView(device, view, nullptr);
        if (image)
            vmaDestroyImage(allocator, image, memory);
    }
};
struct Mesh {
    std::shared_ptr<const MeshAsset> source;
    std::shared_ptr<Buffer> data;
    std::shared_ptr<Mesh> previous;
    size_t copied{}, indexOffset{};
    bool ready{};
};
struct Material {
    VkDevice device{};
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    std::shared_ptr<const MaterialAsset> source;
    uint32_t textureTopMipDrop{};
    std::shared_ptr<Buffer> data;
    std::array<std::shared_ptr<Image>, 5> images;
    std::array<VkSampler, 5> samplers{};
    glm::ivec4 alphaSampler{};
    ~Material() {
        if (set)
            vkFreeDescriptorSets(device, pool, 1, &set);
        for (auto sampler : samplers)
            if (sampler)
                vkDestroySampler(device, sampler, nullptr);
    }
};
struct MaterialUniform {
    glm::vec4 base, emissive, factors, flags;
    std::array<glm::vec4, 5> uv, extra;
};
struct Instance {
    glm::mat4 world;
    glm::vec4 normal0, normal1, normal2, color;
};
struct ViewUniform {
    glm::mat4 vp;
    glm::vec4 eye;
};
static_assert(sizeof(AssetVertex) == 72 && sizeof(MaterialUniform) == 224 && sizeof(Instance) == 128 &&
                  sizeof(ViewUniform) == 80,
              "C++ data layout must match the PBR shader inputs");
void transition(VkCommandBuffer cmd, const Image& image, VkImageLayout from, VkImageLayout to,
                VkPipelineStageFlags2 src, VkAccessFlags2 access, VkPipelineStageFlags2 dst, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.image = image.image;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcStageMask = src;
    b.srcAccessMask = access;
    b.dstStageMask = dst;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                          static_cast<uint32_t>(image.source->mips.size() - image.baseMip), 0, 1};
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    info.imageMemoryBarrierCount = 1;
    info.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &info);
}
VkShaderModule shader(VkDevice device, const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("Missing PBR shader");
    const auto n = in.tellg();
    if (n <= 0 || n % 4)
        throw std::runtime_error("Invalid PBR shader");
    std::vector<uint32_t> data(static_cast<size_t>(n) / 4);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), n);
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = static_cast<size_t>(n);
    info.pCode = data.data();
    VkShaderModule result{};
    vkCheck(vkCreateShaderModule(device, &info, nullptr, &result), "PBR shader");
    return result;
}
} // namespace asset_gpu_detail
using namespace asset_gpu_detail;
struct AssetRenderer::State {
    struct Lighting;
    std::shared_ptr<Lighting> lighting;
    std::filesystem::path shaderDirectory;
    LightingStats stats;
    VulkanContext context;
    VmaAllocator allocator;
    VkDescriptorSetLayout viewLayout{}, materialLayout{};
    VkPipelineLayout layout{};
    std::array<VkPipeline, 4> pipelines{};
    std::vector<VkDescriptorPool> pools;
    struct Frame {
        std::shared_ptr<Buffer> stage, view, instances;
        VkDescriptorSet set{};
        std::vector<std::shared_ptr<void>> retained;
    };
    std::array<Frame, 2> frames;
    std::unordered_map<AssetId, std::shared_ptr<Mesh>> meshes;
    std::unordered_map<std::string, std::shared_ptr<Image>> images;
    std::unordered_map<AssetId, std::shared_ptr<Material>> materials;
    std::shared_ptr<AssetCatalog> catalog;
    std::unordered_map<AssetId, std::shared_ptr<const MeshAsset>> nativeMeshes;
    std::shared_ptr<MaterialAsset> nativeMaterial = std::make_shared<MaterialAsset>();
    size_t bytes{}, waiting{}, lastAssetUploadBytes{};
    uint32_t textureTopMipDrop{};
    std::string failure;
    std::shared_ptr<TexturePixels> white = std::make_shared<TexturePixels>(),
                                   normal = std::make_shared<TexturePixels>();
    State(const VulkanContext& c, VmaAllocator a) : context(c), allocator(a) {
        white->hash = "builtin-white";
        white->mips = {{1, 1, {255, 255, 255, 255}}};
        normal->hash = "builtin-normal";
        normal->mips = {{1, 1, {128, 128, 255, 255}}};
        nativeMaterial->id = AssetId::parse("00000000-0000-4000-8000-000000000101");
        nativeMaterial->name = "Primitive PBR";
        nativeMaterial->values.metallic = 0;
        nativeMaterial->values.roughness = .65f;
        const auto geometry = makePrimitives();
        for (size_t primitive = 0; primitive < 2; ++primitive) {
            auto mesh = std::make_shared<MeshAsset>();
            mesh->id = AssetId::parse(primitive ? "00000000-0000-4000-8000-000000000002"
                                                : "00000000-0000-4000-8000-000000000001");
            mesh->name = primitive ? "Plane" : "Cube";
            mesh->revision = "native-pbr-1";
            const auto range = geometry.ranges[primitive];
            for (uint32_t face = 0; face < range.indexCount; face += 6) {
                const auto first = geometry.indices[range.firstIndex + face];
                const auto base = static_cast<uint32_t>(mesh->vertices.size());
                const auto a = geometry.vertices[first].position, b = geometry.vertices[first + 1].position,
                           c = geometry.vertices[first + 2].position;
                const auto n = glm::normalize(glm::cross(b - a, c - a)), t = glm::normalize(b - a);
                const glm::vec2 uv[]{{0, 1}, {1, 1}, {1, 0}, {0, 0}};
                for (uint32_t vertex = 0; vertex < 4; ++vertex) {
                    AssetVertex v;
                    v.position = geometry.vertices[first + vertex].position;
                    v.normal = n;
                    v.tangent = {t, -1};
                    v.uv0 = uv[vertex];
                    mesh->vertices.push_back(v);
                }
                for (uint32_t i : {0u, 1u, 2u, 2u, 3u, 0u})
                    mesh->indices.push_back(base + i);
            }
            mesh->parts = {{0, static_cast<uint32_t>(mesh->indices.size()), nativeMaterial->id}};
            mesh->bounds = {glm::vec3(-.5f), glm::vec3(.5f)};
            if (primitive)
                mesh->bounds.min.y = mesh->bounds.max.y = 0;
            nativeMeshes[mesh->id] = mesh;
        }
    }
    ~State() {
        lighting.reset();
        for (auto& f : frames) {
            f.retained.clear();
            f.stage.reset();
            f.view.reset();
            f.instances.reset();
        }
        materials.clear();
        meshes.clear();
        images.clear();
        for (auto p : pipelines)
            if (p)
                vkDestroyPipeline(context.device, p, nullptr);
        if (layout)
            vkDestroyPipelineLayout(context.device, layout, nullptr);
        for (auto p : pools)
            vkDestroyDescriptorPool(context.device, p, nullptr);
        if (viewLayout)
            vkDestroyDescriptorSetLayout(context.device, viewLayout, nullptr);
        if (materialLayout)
            vkDestroyDescriptorSetLayout(context.device, materialLayout, nullptr);
    }
    std::shared_ptr<const MeshAsset> meshSource(AssetId id) const {
        if (auto it = nativeMeshes.find(id); it != nativeMeshes.end())
            return it->second;
        return catalog->meshes.at(id);
    }
    std::shared_ptr<const MaterialAsset> materialSource(AssetId id) const {
        return id == nativeMaterial->id ? nativeMaterial : catalog->materials.at(id);
    }
    VkDescriptorSet allocate(VkDescriptorSetLayout layout, VkDescriptorPool& pool) {
        if (!pools.empty()) {
            pool = pools.back();
            VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            a.descriptorPool = pool;
            a.descriptorSetCount = 1;
            a.pSetLayouts = &layout;
            VkDescriptorSet set{};
            const auto result = vkAllocateDescriptorSets(context.device, &a, &set);
            if (result == VK_SUCCESS)
                return set;
            if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
                vkCheck(result, "Asset descriptors");
        }
        const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 128},
                                           {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 640}};
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = 128;
        info.poolSizeCount = 2;
        info.pPoolSizes = sizes;
        vkCheck(vkCreateDescriptorPool(context.device, &info, nullptr, &pool), "Asset descriptor pool");
        pools.push_back(pool);
        return allocate(layout, pool);
    }
    std::shared_ptr<Image> image(std::shared_ptr<const TexturePixels> source) {
        if (allocationFailureForTesting.load())
            throw std::runtime_error("Injected AssetRenderer allocation failure");
        const auto key = source->hash + ":" + std::to_string(textureTopMipDrop);
        if (auto it = images.find(key); it != images.end())
            return it->second;
        auto out = std::make_shared<Image>();
        out->device = context.device;
        out->allocator = allocator;
        out->source = source;
        out->baseMip = std::min<size_t>(textureTopMipDrop, source->mips.size() - 1);
        out->mip = out->baseMip;
        const auto& base = source->mips[out->baseMip];
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = source->srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {base.width, base.height, 1};
        info.mipLevels = static_cast<uint32_t>(source->mips.size() - out->baseMip);
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        vkCheck(vmaCreateImage(allocator, &info, &memory, &out->image, &out->memory, nullptr), "Asset image");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = out->image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = info.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, info.mipLevels, 0, 1};
        vkCheck(vkCreateImageView(context.device, &view, nullptr, &out->view), "Asset image view");
        images[key] = out;
        return out;
    }
    std::shared_ptr<Mesh> mesh(AssetId id) {
        auto source = meshSource(id);
        auto& current = meshes[id];
        if (current && current->source->revision == source->revision)
            return current;
        auto out = std::make_shared<Mesh>();
        out->source = source;
        out->indexOffset = source->vertices.size() * sizeof(AssetVertex);
        out->data = buffer(context.device, allocator, out->indexOffset + source->indices.size() * 4,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           false);
        out->previous = current;
        current = out;
        return out;
    }
    std::shared_ptr<Material> material(AssetId id) {
        auto source = materialSource(id);
        auto old = materials.find(id);
        if (old != materials.end() && old->second->source == source &&
            old->second->textureTopMipDrop == textureTopMipDrop)
            return old->second;
        std::array<std::shared_ptr<Image>, 5> textures;
        for (size_t i = 0; i < 5; ++i) {
            const auto ref = source->textures[i].texture;
            textures[i] = image(ref ? catalog->textures.at(ref)->pixels : (i == 2 ? normal : white));
            if (!textures[i]->ready)
                return old == materials.end() ? nullptr : old->second;
        }
        auto m = std::make_shared<Material>();
        m->device = context.device;
        m->source = source;
        m->textureTopMipDrop = textureTopMipDrop;
        m->images = textures;
        m->set = allocate(materialLayout, m->pool);
        m->data = buffer(context.device, allocator, sizeof(MaterialUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        const auto& v = source->values;
        MaterialUniform value{v.baseColor,
                              glm::vec4(v.emissive, 0),
                              {v.metallic, v.roughness, v.normalScale, v.occlusion},
                              {float(v.mask), v.alphaCutoff, float(v.unlit), 0},
                              {},
                              {}};
        for (size_t i = 0; i < 5; ++i) {
            const auto& s = source->textures[i];
            value.uv[i] = {s.scale, s.offset};
            value.extra[i] = {s.rotation, float(s.texCoord), 0, 0};
            TextureAsset t;
            if (s.texture)
                t = *catalog->textures.at(s.texture);
            if (i == 0)
                m->alphaSampler = {t.minFilter, t.magFilter, t.wrapS, t.wrapT};
            auto address = [](int v) {
                return v == 33071   ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                       : v == 33648 ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
                                    : VK_SAMPLER_ADDRESS_MODE_REPEAT;
            };
            VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sampler.magFilter = t.magFilter == 9728 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            sampler.minFilter = (t.minFilter == 9728 || t.minFilter == 9984 || t.minFilter == 9986) ? VK_FILTER_NEAREST
                                                                                                    : VK_FILTER_LINEAR;
            sampler.mipmapMode = (t.minFilter == 9984 || t.minFilter == 9985) ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                                                              : VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sampler.addressModeU = address(t.wrapS);
            sampler.addressModeV = address(t.wrapT);
            sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sampler.maxLod = t.minFilter < 9984
                                ? 0
                                : float(textures[i]->source->mips.size() - textures[i]->baseMip - 1);
            vkCheck(vkCreateSampler(context.device, &sampler, nullptr, &m->samplers[i]), "Asset sampler");
        }
        std::memcpy(m->data->mapped, &value, sizeof(value));
        vkCheck(vmaFlushAllocation(allocator, m->data->memory, 0, VK_WHOLE_SIZE), "Material flush");
        VkDescriptorBufferInfo b{m->data->buffer, 0, sizeof(value)};
        std::array<VkDescriptorImageInfo, 5> is{};
        std::array<VkWriteDescriptorSet, 6> writes{};
        for (size_t i = 0; i < 6; ++i) {
            auto& w = writes[i];
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = m->set;
            w.dstBinding = static_cast<uint32_t>(i);
            w.descriptorCount = 1;
            w.descriptorType = i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            if (i) {
                is[i - 1] = {m->samplers[i - 1], textures[i - 1]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                w.pImageInfo = &is[i - 1];
            } else
                w.pBufferInfo = &b;
        }
        vkUpdateDescriptorSets(context.device, 6, writes.data(), 0, nullptr);
        materials[id] = m;
        return m;
    }
};
AssetRenderer::AssetRenderer(const VulkanContext& c, VmaAllocator a, const std::filesystem::path& directory)
    : state_(std::make_unique<State>(c, a)) {
    state_->shaderDirectory = directory;
    auto& s = *state_;
    VkDescriptorSetLayoutBinding view{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 1;
    info.pBindings = &view;
    vkCheck(vkCreateDescriptorSetLayout(c.device, &info, nullptr, &s.viewLayout), "PBR view layout");
    std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
    for (uint32_t i = 0; i < 6; ++i)
        bindings[i] = {i, i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    info.bindingCount = 6;
    info.pBindings = bindings.data();
    vkCheck(vkCreateDescriptorSetLayout(c.device, &info, nullptr, &s.materialLayout), "PBR material layout");
    const VkDescriptorSetLayout layouts[]{s.viewLayout, s.materialLayout};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 2;
    li.pSetLayouts = layouts;
    vkCheck(vkCreatePipelineLayout(c.device, &li, nullptr, &s.layout), "PBR pipeline layout");
    const VkShaderModule vert = shader(c.device, directory / "pbr.vert.spv"),
                         frag = shader(c.device, directory / "pbr.frag.spv");
    struct Clean {
        VkDevice d;
        VkShaderModule v, f;
        ~Clean() {
            vkDestroyShaderModule(d, v, nullptr);
            vkDestroyShaderModule(d, f, nullptr);
        }
    } clean{c.device, vert, frag};
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    const VkVertexInputBindingDescription vb[]{{0, sizeof(AssetVertex), VK_VERTEX_INPUT_RATE_VERTEX},
                                               {1, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE}};
    std::array<VkVertexInputAttributeDescription, 14> attrs{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(AssetVertex, position)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(AssetVertex, normal)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(AssetVertex, tangent)};
    attrs[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(AssetVertex, color)};
    attrs[4] = {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(AssetVertex, uv0)};
    attrs[5] = {5, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(AssetVertex, uv1)};
    for (uint32_t i = 0; i < 8; ++i)
        attrs[i + 6] = {i + 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 2;
    vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = 14;
    vi.pVertexAttributeDescriptions = attrs.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
    const VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color;
    rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.pNext = &rendering;
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vi;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &vp;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &samples;
    pipeline.pDepthStencilState = &depth;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = s.layout;
    for (size_t i = 0; i < 4; ++i) {
        raster.cullMode = i >= 2 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        raster.frontFace = (i % 2) == 1 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        vkCheck(vkCreateGraphicsPipelines(c.device, {}, 1, &pipeline, nullptr, &s.pipelines[i]), "PBR pipeline");
    }
}
AssetRenderer::~AssetRenderer() = default;
void AssetRenderer::upload(size_t frame, VkCommandBuffer cmd, const RenderView& view) {
    auto& s = *state_;
    auto& f = s.frames[frame];
    if (asset_gpu_detail::allocationFailureForTesting.load())
        throw std::runtime_error("Injected AssetRenderer allocation failure");
    f.retained.clear();
    s.bytes = s.waiting = 0;
    s.lastAssetUploadBytes = 0;
    s.stats.assetUploadBytes = 0;
    s.textureTopMipDrop = view.runtimeLighting.explicitSettings ? view.runtimeLighting.textureTopMipDrop : 0;
    const auto mipSuffix = ":" + std::to_string(s.textureTopMipDrop);
    for (auto it = s.images.begin(); it != s.images.end();) {
        if (it->first.size() < mipSuffix.size() ||
            it->first.compare(it->first.size() - mipSuffix.size(), mipSuffix.size(), mipSuffix) == 0 ||
            it->second.use_count() != 1)
            ++it;
        else
            it = s.images.erase(it);
    }
    for (auto it = s.materials.begin(); it != s.materials.end();) {
        if (it->second->textureTopMipDrop == s.textureTopMipDrop || it->second.use_count() != 1)
            ++it;
        else
            it = s.materials.erase(it);
    }
    if (s.catalog != view.assets) {
        s.materials.clear();
        s.meshes.clear();
        s.images.clear();
        s.catalog = view.assets;
        s.failure.clear();
    }
    if ((view.imported.empty() && view.casters.empty()) || !s.catalog)
        return;
    try {
        if (!f.stage)
            f.stage = buffer(s.context.device, s.allocator, uploadBudget, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        std::set<AssetId> neededMeshes, neededMaterials;
        const auto collect = [&](const ImportedItem& item) {
            neededMeshes.insert(item.mesh);
            const auto& mesh = *s.meshSource(item.mesh);
            for (size_t i = 0; i < mesh.parts.size(); ++i)
                neededMaterials.insert(item.materials.empty() ? mesh.parts[i].material : item.materials.at(i));
        };
        for (const auto& item : view.imported)
            collect(item);
        for (const auto& item : view.casters)
            collect(item);
        for (auto id : neededMaterials) {
            auto m = s.materialSource(id);
            for (size_t i = 0; i < 5; ++i) {
                auto ref = m->textures[i].texture;
                s.image(ref ? s.catalog->textures.at(ref)->pixels : (i == 2 ? s.normal : s.white));
            }
        }
        for (auto id : neededMeshes) {
            auto m = s.mesh(id);
            if (m->ready)
                continue;
            const size_t remaining = uploadBudget - s.bytes;
            const size_t amount = std::min(remaining, m->data->size - m->copied) & ~size_t(3);
            if (!amount) {
                ++s.waiting;
                continue;
            }
            size_t at = m->copied, left = amount, dst = s.bytes;
            while (left) {
                const bool vertices = at < m->indexOffset;
                const size_t available = vertices ? m->indexOffset - at : m->data->size - at;
                const auto* p =
                    vertices ? reinterpret_cast<const uint8_t*>(m->source->vertices.data()) + at
                             : reinterpret_cast<const uint8_t*>(m->source->indices.data()) + (at - m->indexOffset);
                const size_t n = std::min(left, available);
                std::memcpy(static_cast<char*>(f.stage->mapped) + dst, p, n);
                at += n;
                dst += n;
                left -= n;
            }
            VkBufferCopy copy{s.bytes, m->copied, amount};
            vkCmdCopyBuffer(cmd, f.stage->buffer, m->data->buffer, 1, &copy);
            m->copied += amount;
            s.bytes += amount;
            f.retained.push_back(m);
            if (m->copied == m->data->size) {
                VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
                b.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT;
                b.buffer = m->data->buffer;
                b.size = VK_WHOLE_SIZE;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                d.bufferMemoryBarrierCount = 1;
                d.pBufferMemoryBarriers = &b;
                vkCmdPipelineBarrier2(cmd, &d);
                m->ready = true;
                m->previous.reset();
            } else
                ++s.waiting;
        }
        for (auto& [key, image] : s.images) {
            if (image->ready)
                continue;
            bool touched = false;
            while (image->mip < image->source->mips.size()) {
                const auto& level = image->source->mips[image->mip];
                const size_t pitch = size_t(level.width) * 4,
                             rows = std::min<size_t>((uploadBudget - s.bytes) / pitch, level.height - image->row);
                if (!rows)
                    break;
                if (image->mip == image->baseMip && image->row == 0)
                    transition(cmd, *image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                               VK_ACCESS_2_TRANSFER_WRITE_BIT);
                std::memcpy(static_cast<char*>(f.stage->mapped) + s.bytes, level.rgba.data() + image->row * pitch,
                            rows * pitch);
                VkBufferImageCopy copy{};
                copy.bufferOffset = s.bytes;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,
                                         static_cast<uint32_t>(image->mip - image->baseMip), 0, 1};
                copy.imageOffset = {0, static_cast<int32_t>(image->row), 0};
                copy.imageExtent = {level.width, static_cast<uint32_t>(rows), 1};
                vkCmdCopyBufferToImage(cmd, f.stage->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                       &copy);
                s.bytes += rows * pitch;
                image->row += rows;
                touched = true;
                if (image->row == level.height) {
                    image->row = 0;
                    ++image->mip;
                }
            }
            if (touched) {
                f.retained.push_back(image);
            }
            if (image->mip == image->source->mips.size()) {
                transition(cmd, *image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                image->ready = true;
            } else
                ++s.waiting;
        }
        if (s.bytes)
            vkCheck(vmaFlushAllocation(s.allocator, f.stage->memory, 0, s.bytes), "Staging flush");
    } catch (const std::exception& e) {
        s.failure = e.what();
    }
    s.lastAssetUploadBytes = s.bytes;
    s.stats.assetUploadBytes = s.bytes;
    if (asset_gpu_detail::allocationFailureAfterUploadForTesting.load())
        throw std::runtime_error("Injected late AssetRenderer upload failure");
}
void AssetRenderer::draw(size_t frame, VkCommandBuffer cmd, const RenderView& view) {
    auto& s = *state_;
    if (view.imported.empty() || !s.failure.empty())
        return;
    auto& f = s.frames[frame];
    struct Draw {
        std::shared_ptr<Mesh> mesh;
        std::shared_ptr<Material> material;
        MeshPart part;
        size_t pipeline{};
        std::vector<Instance> instances;
    };
    std::map<std::tuple<AssetId, uint32_t, AssetId, size_t>, Draw> groups;
    for (auto& item : view.imported) {
        auto mesh = s.meshes.at(item.mesh);
        if (!mesh->ready)
            mesh = mesh->previous;
        if (!mesh || !mesh->ready)
            continue;
        for (size_t i = 0; i < mesh->source->parts.size(); ++i) {
            const auto part = mesh->source->parts[i];
            auto id = item.materials.empty() ? part.material : item.materials.at(i);
            auto mat = s.material(id);
            if (!mat)
                continue;
            const float sign = glm::determinant(glm::mat3(item.world)) < 0 ? -1.0f : 1.0f;
            const size_t pipeline = (mat->source->values.doubleSided ? 2u : 0u) + (sign < 0 ? 1u : 0u);
            auto& group = groups[{item.mesh, static_cast<uint32_t>(i), id, pipeline}];
            group.mesh = mesh;
            group.material = mat;
            group.part = part;
            group.pipeline = pipeline;
            group.instances.push_back(
                {item.world, {item.normal[0], sign}, {item.normal[1], 0}, {item.normal[2], 0}, item.color});
        }
    }
    size_t count{};
    for (auto& [key, g] : groups)
        count += g.instances.size();
    if (!count)
        return;
    if (!f.view) {
        f.view = buffer(s.context.device, s.allocator, sizeof(ViewUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        VkDescriptorPool pool{};
        f.set = s.allocate(s.viewLayout, pool);
        VkDescriptorBufferInfo b{f.view->buffer, 0, sizeof(ViewUniform)};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = f.set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &b;
        vkUpdateDescriptorSets(s.context.device, 1, &w, 0, nullptr);
    }
    ViewUniform camera{view.viewProjection, glm::vec4(view.eye, 1)};
    std::memcpy(f.view->mapped, &camera, sizeof(camera));
    vkCheck(vmaFlushAllocation(s.allocator, f.view->memory, 0, VK_WHOLE_SIZE), "PBR view flush");
    if (!f.instances || f.instances->size < count * sizeof(Instance))
        f.instances = buffer(s.context.device, s.allocator, std::max<size_t>(64, count * 2) * sizeof(Instance),
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    size_t first{};
    for (auto& [key, g] : groups) {
        std::memcpy(static_cast<Instance*>(f.instances->mapped) + first, g.instances.data(),
                    g.instances.size() * sizeof(Instance));
        const VkBuffer buffers[]{g.mesh->data->buffer, f.instances->buffer};
        const VkDeviceSize offsets[]{0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
        vkCmdBindIndexBuffer(cmd, g.mesh->data->buffer, g.mesh->indexOffset, VK_INDEX_TYPE_UINT32);
        const VkDescriptorSet sets[]{f.set, g.material->set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 2, sets, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipelines[g.pipeline]);
        vkCmdDrawIndexed(cmd, g.part.indexCount, static_cast<uint32_t>(g.instances.size()), g.part.firstIndex, 0,
                         static_cast<uint32_t>(first));
        ++s.stats.legacyDrawCalls;
        ++s.stats.drawCalls;
        ++s.stats.totalDrawCalls;
        first += g.instances.size();
        f.retained.push_back(g.mesh);
        f.retained.push_back(g.material);
    }
    vkCheck(vmaFlushAllocation(s.allocator, f.instances->memory, 0, count * sizeof(Instance)), "PBR instance flush");
}
size_t AssetRenderer::pending() const {
    return state_->waiting;
}
size_t AssetRenderer::uploadedBytes() const {
    return state_->bytes;
}
size_t AssetRenderer::meshCount() const {
    return state_->meshes.size();
}
size_t AssetRenderer::imageCount() const {
    return state_->images.size();
}
const std::string& AssetRenderer::error() const {
    return state_->failure;
}
void AssetRenderer::clearError() {
    state_->failure.clear();
}
void AssetRenderer::setAllocationFailureForTesting(bool enabled) {
    asset_gpu_detail::allocationFailureForTesting.store(enabled);
    if (!enabled) {
        asset_gpu_detail::allocationFailureAfterUploadForTesting.store(false);
        asset_gpu_detail::allocationFailureAfterLightingForTesting.store(false);
    }
}
void AssetRenderer::setAllocationFailureAfterUploadForTesting(bool enabled) {
    asset_gpu_detail::allocationFailureAfterUploadForTesting.store(enabled);
}
void AssetRenderer::setAllocationFailureAfterLightingForTesting(bool enabled) {
    asset_gpu_detail::allocationFailureAfterLightingForTesting.store(enabled);
}
#include "renderer/AssetLighting.inl"
void AssetRenderer::abortFrame(size_t frame) noexcept {
    if (!state_ || frame >= state_->frames.size())
        return;
    state_->frames[frame].retained.clear();
    state_->bytes = state_->waiting = 0;
    state_->lastAssetUploadBytes = 0;
    state_->failure.clear();
    if (state_->lighting)
        state_->lighting->abortFrame(frame);
    // Mesh/image/material readiness describes commands that were discarded.
    // Clearing the maps forces the next submitted frame to recreate and
    // upload them, while shared references held by the other frame protect
    // resources still used by the GPU.
    state_->materials.clear();
    state_->meshes.clear();
    state_->images.clear();
}
} // namespace proto
