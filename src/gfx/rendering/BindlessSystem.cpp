#include "BindlessSystem.hpp"
#include <stdexcept>
#include <array>
#include <cstring>

namespace gfx {

BindlessSystem::BindlessSystem(VulkanContext& context) : m_context(context) {
    createObjectBuffers();
    createDescriptorPool();
    createDescriptorSetLayout();
    createDescriptorSets();
}

BindlessSystem::~BindlessSystem() {
    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        if (m_objectBuffers[i]) {
            m_objectBuffers[i]->unmap();
            delete m_objectBuffers[i];
        }
        if (m_paletteBuffers[i]) {
            m_paletteBuffers[i]->unmap();
            delete m_paletteBuffers[i];
        }
    }
    vkDestroyDescriptorPool(m_context.getDevice(), m_descriptorPool, nullptr);
    vkDestroyDescriptorSetLayout(m_context.getDevice(), m_descriptorSetLayout, nullptr);
}

void BindlessSystem::createObjectBuffers() {
    VkDeviceSize bufferSize = m_maxObjects * sizeof(ObjectDataSSBO);
    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        m_objectBuffers[i] = new Buffer(
            m_context, bufferSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        m_objectBuffers[i]->map(&m_objectBuffersMapped[i]);

        m_paletteBuffers[i] = new Buffer(
            m_context, sizeof(PaletteUBO),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        m_paletteBuffers[i]->map(&m_paletteBuffersMapped[i]);
    }
}

void BindlessSystem::createDescriptorPool() {
    std::array<VkDescriptorPoolSize, 3> poolSizes{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = MAX_BINDLESS_RESOURCES * MAX_FRAMES;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = MAX_FRAMES;
    poolSizes[2].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[2].descriptorCount = MAX_FRAMES;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();
    poolInfo.maxSets       = MAX_FRAMES;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;

    if (vkCreateDescriptorPool(m_context.getDevice(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create bindless descriptor pool!");
}

void BindlessSystem::createDescriptorSetLayout() {
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = MAX_BINDLESS_RESOURCES;
    bindings[0].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    
    bindings[1].binding         = 1;
    bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    bindings[2].binding         = 2;
    bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    std::array<VkDescriptorBindingFlags, 3> bindingFlags{};
    bindingFlags[0] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    bindingFlags[1] = 0;
    bindingFlags[2] = 0;

    VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsInfo{};
    bindingFlagsInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    bindingFlagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
    bindingFlagsInfo.pBindingFlags = bindingFlags.data();

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings    = bindings.data();
    layoutInfo.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.pNext        = &bindingFlagsInfo;

    if (vkCreateDescriptorSetLayout(m_context.getDevice(), &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS)
        throw std::runtime_error("failed to create bindless descriptor set layout!");
}

void BindlessSystem::createDescriptorSets() {
    std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES, m_descriptorSetLayout);
    m_descriptorSets.resize(MAX_FRAMES);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = m_descriptorPool;
    allocInfo.descriptorSetCount = MAX_FRAMES;
    allocInfo.pSetLayouts        = layouts.data();

    if (vkAllocateDescriptorSets(m_context.getDevice(), &allocInfo, m_descriptorSets.data()) != VK_SUCCESS)
        throw std::runtime_error("failed to allocate bindless descriptor sets!");

    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_objectBuffers[i]->getBuffer();
        bufferInfo.offset = 0;
        bufferInfo.range  = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writes[2]{};

        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = m_descriptorSets[i];
        writes[0].dstBinding      = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &bufferInfo;

        VkDescriptorBufferInfo paletteInfo{};
        paletteInfo.buffer = m_paletteBuffers[i]->getBuffer();
        paletteInfo.offset = 0;
        paletteInfo.range  = sizeof(PaletteUBO);

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = m_descriptorSets[i];
        writes[1].dstBinding      = 2;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo     = &paletteInfo;

        vkUpdateDescriptorSets(m_context.getDevice(), 2, writes, 0, nullptr);
    }
}

uint32_t BindlessSystem::registerTexture(VkImageView imageView, VkSampler sampler) {
    uint32_t index;
    if (!m_freeIndices.empty()) { index = m_freeIndices.front(); m_freeIndices.pop(); }
    else {
        if (m_nextFreeIndex >= MAX_BINDLESS_RESOURCES)
            throw std::runtime_error("Max bindless resources exceeded!");
        index = m_nextFreeIndex++;
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView   = imageView;
    imageInfo.sampler     = sampler;

    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(MAX_FRAMES);
    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        VkWriteDescriptorSet w{};
        w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet          = m_descriptorSets[i];
        w.dstBinding      = 0;
        w.dstArrayElement = index;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo      = &imageInfo;
        writes.push_back(w);
    }
    vkUpdateDescriptorSets(m_context.getDevice(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return index;
}

void BindlessSystem::unregisterTexture(uint32_t id) {
    m_freeIndices.push(id);
}

void BindlessSystem::bind(VkCommandBuffer commandBuffer, VkPipelineLayout pipelineLayout,
                          uint32_t frameIndex, uint32_t set) {
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, set, 1, &m_descriptorSets[frameIndex], 0, nullptr);
}

void BindlessSystem::updateObject(uint32_t frameIndex, uint32_t objectIndex, const ObjectDataSSBO& data) {
    if (objectIndex >= m_maxObjects) return;
    ObjectDataSSBO* mappedArr = static_cast<ObjectDataSSBO*>(m_objectBuffersMapped[frameIndex]);
    mappedArr[objectIndex] = data;
    m_objectBuffers[frameIndex]->flush(objectIndex * sizeof(ObjectDataSSBO), sizeof(ObjectDataSSBO));
}

void BindlessSystem::updatePalette(uint32_t frameIndex, const PaletteUBO& palette) {
    if (!m_paletteBuffersMapped[frameIndex]) return;
    std::memcpy(m_paletteBuffersMapped[frameIndex], &palette, sizeof(PaletteUBO));
    m_paletteBuffers[frameIndex]->flush(0, sizeof(PaletteUBO));
}

} // namespace gfx
