#pragma once
#include "../core/VulkanContext.hpp"

namespace gfx {

class Buffer {
public:
    Buffer(VulkanContext& context, VkDeviceSize size, VkBufferUsageFlags usage,
           VmaMemoryUsage memoryUsage, VmaAllocationCreateFlags allocFlags = 0);
    ~Buffer();

    void map(void** ppData);
    void unmap();
    void flush(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE);
    void upload(const void* data, size_t size);

    [[nodiscard]] VkBuffer getBuffer() const { return buffer; }
    [[nodiscard]] uint64_t getDeviceAddress() const;

private:
    VulkanContext& context;
    VkBuffer buffer;
    VmaAllocation allocation;
    VkDeviceSize size;
};

} // namespace gfx
