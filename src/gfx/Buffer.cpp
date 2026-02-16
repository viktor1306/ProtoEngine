#include "Buffer.hpp"
#include <cstring>
#include <stdexcept>

gfx::Buffer::Buffer(VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags allocFlags)
    : context(context), size(size) {
    
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    
    // Crucial for BDA: Shader must be able to retrieve the address
    if ((usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) || (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) || (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) {
        bufferInfo.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }

    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = memoryUsage;
    allocInfo.flags = allocFlags;

    if (vmaCreateBuffer(context.getAllocator(), &bufferInfo, &allocInfo, &buffer, &allocation, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("failed to allocate buffer!");
    }
}

gfx::Buffer::~Buffer() {
    vmaDestroyBuffer(context.getAllocator(), buffer, allocation);
}

void gfx::Buffer::map(void** ppData) {
    vmaMapMemory(context.getAllocator(), allocation, ppData);
}

void gfx::Buffer::unmap() {
    vmaUnmapMemory(context.getAllocator(), allocation);
}

void gfx::Buffer::flush(VkDeviceSize offset, VkDeviceSize size) {
    vmaFlushAllocation(context.getAllocator(), allocation, offset, size);
}

void gfx::Buffer::upload(const void* data, size_t size) {
    void* mappedData;
    map(&mappedData);
    std::memcpy(mappedData, data, size);
    flush(); 
    unmap();
}

uint64_t gfx::Buffer::getDeviceAddress() const {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(context.getDevice(), &info);
}
