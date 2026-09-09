#include "renderer/VulkanRenderer.hpp"
#include "renderer/AssetRenderer.hpp"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace proto {
void vkCheck(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (VkResult " + std::to_string(result) + ')');
}
namespace {
VKAPI_ATTR VkBool32 VKAPI_CALL debugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT type,
                                            const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto& log = *static_cast<Diagnostics*>(user);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++log.validationErrors;
        log.write("VULKAN ERROR", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        if (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) {
            ++log.validationWarnings;
            log.write("VULKAN WARNING", data->pMessage);
        } else {
            ++log.loaderWarnings;
            log.write("VULKAN LOADER", data->pMessage);
        }
    }
    return VK_FALSE;
}
bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(),
                       [=](const auto& ext) { return std::strcmp(ext.extensionName, name) == 0; });
}
void barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout oldLayout,
             VkImageLayout newLayout, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 imageBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    imageBarrier.srcStageMask = srcStage;
    imageBarrier.srcAccessMask = srcAccess;
    imageBarrier.dstStageMask = dstStage;
    imageBarrier.dstAccessMask = dstAccess;
    imageBarrier.oldLayout = oldLayout;
    imageBarrier.newLayout = newLayout;
    imageBarrier.srcQueueFamilyIndex = imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.image = image;
    imageBarrier.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &imageBarrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}
struct Shader {
    VkDevice device;
    VkShaderModule handle{};
    Shader(VkDevice dev, const std::filesystem::path& path) : device(dev) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            throw std::runtime_error("Missing compiled shader: " + utf8(path.wstring()));
        const auto bytes = file.tellg();
        if (bytes <= 0 || bytes % 4 != 0)
            throw std::runtime_error("Invalid SPIR-V size");
        std::vector<uint32_t> code(static_cast<size_t>(bytes) / 4);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(code.data()), bytes);
        if (!file || code[0] != 0x07230203)
            throw std::runtime_error("Invalid SPIR-V file");
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = static_cast<size_t>(bytes);
        info.pCode = code.data();
        vkCheck(vkCreateShaderModule(device, &info, nullptr, &handle), "vkCreateShaderModule");
    }
    ~Shader() {
        if (handle)
            vkDestroyShaderModule(device, handle, nullptr);
    }
};
struct ReadbackBuffer {
    VmaAllocator allocator;
    VkBuffer buffer{};
    VmaAllocation allocation{};
    void* data{};
    ReadbackBuffer(VmaAllocator owner, VkDeviceSize bytes) : allocator(owner) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        memory.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo result{};
        vkCheck(vmaCreateBuffer(allocator, &info, &memory, &buffer, &allocation, &result), "Readback buffer");
        data = result.pMappedData;
    }
    ~ReadbackBuffer() {
        if (buffer)
            vmaDestroyBuffer(allocator, buffer, allocation);
    }
    void invalidate() {
        vkCheck(vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE), "Invalidate readback");
    }
};
void copyImage(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkExtent2D extent, VkBuffer buffer) {
    VkBufferImageCopy region{};
    region.imageSubresource = {aspect, 0, 0, 1};
    region.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
}
} // namespace

VulkanRenderer::VulkanRenderer(Diagnostics& log) : log_(log) {}
VulkanRenderer::~VulkanRenderer() {
    if (context_.device)
        vkDeviceWaitIdle(context_.device);
    assetRenderer_.reset();
    for (auto& frame : frames_) {
        if (frame.instances)
            vmaDestroyBuffer(allocator_, frame.instances, frame.instanceMemory);
        destroyTarget(frame.target);
        if (frame.queries)
            vkDestroyQueryPool(context_.device, frame.queries, nullptr);
        if (frame.acquired)
            vkDestroySemaphore(context_.device, frame.acquired, nullptr);
        if (frame.fence)
            vkDestroyFence(context_.device, frame.fence, nullptr);
        if (frame.pool)
            vkDestroyCommandPool(context_.device, frame.pool, nullptr);
    }
    if (pipeline_)
        vkDestroyPipeline(context_.device, pipeline_, nullptr);
    for (const auto pipeline : scenePipelines_)
        if (pipeline)
            vkDestroyPipeline(context_.device, pipeline, nullptr);
    if (sceneLayout_)
        vkDestroyPipelineLayout(context_.device, sceneLayout_, nullptr);
    if (geometry_)
        vmaDestroyBuffer(allocator_, geometry_, geometryMemory_);
    if (pipelineLayout_)
        vkDestroyPipelineLayout(context_.device, pipelineLayout_, nullptr);
    destroySwapchain();
    if (allocator_)
        vmaDestroyAllocator(allocator_);
    if (context_.device)
        vkDestroyDevice(context_.device, nullptr);
    if (surface_)
        vkDestroySurfaceKHR(context_.instance, surface_, nullptr);
    if (messenger_)
        vkDestroyDebugUtilsMessengerEXT(context_.instance, messenger_, nullptr);
    if (context_.instance)
        vkDestroyInstance(context_.instance, nullptr);
    log_.write("INFO", "Vulkan resources released");
}
VulkanRenderer::AcquiredFrameRecovery::~AcquiredFrameRecovery() noexcept {
    if (!active)
        return;
    try {
        if (renderer.assetRenderer_)
            renderer.assetRenderer_->abortFrame(renderer.frameIndex_);
        renderer.recoverAcquiredFrame(frame, swap, imageIndex);
        target.rendered = targetRenderedBefore;
        renderer.sceneRenderFrames_ = sceneRenderFramesBefore;
    } catch (...) {
        // Recovery runs during exception unwinding. The original Vulkan or
        // allocator exception remains the useful diagnostic.
    }
}
void VulkanRenderer::initialize(GLFWwindow* window, const std::filesystem::path& shaderDirectory, bool validation) {
    window_ = window;
    validation_ = validation;
    createInstance();
    vkCheck(glfwCreateWindowSurface(context_.instance, window_, nullptr, &surface_), "GLFW Vulkan surface");
    createDevice();
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo info{};
    info.flags = memoryBudget_ ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0;
    info.instance = context_.instance;
    info.physicalDevice = context_.physicalDevice;
    info.device = context_.device;
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.pVulkanFunctions = &functions;
    vkCheck(vmaCreateAllocator(&info, &allocator_), "VMA allocator");
    createSwapchain();
    createFrames();
    createPipeline(shaderDirectory);
    createGeometry();
    createScenePipelines(shaderDirectory);
    assetRenderer_ = std::make_unique<AssetRenderer>(context_, allocator_, shaderDirectory);
}
void VulkanRenderer::createInstance() {
    if (!vkEnumerateInstanceVersion)
        throw std::runtime_error("Vulkan 1.3 loader required; installed loader only exposes Vulkan 1.0");
    uint32_t loaderVersion{};
    vkCheck(vkEnumerateInstanceVersion(&loaderVersion), "Vulkan loader version");
    if (loaderVersion < VK_API_VERSION_1_3)
        throw std::runtime_error("Proto Engine requires a Vulkan 1.3 loader/driver");
    uint32_t count{};
    const char** required = glfwGetRequiredInstanceExtensions(&count);
    if (!required || !count)
        throw std::runtime_error("GLFW cannot find Vulkan surface extensions");
    std::vector<const char*> extensions(required, required + count);
    vkCheck(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr), "Instance extension count");
    std::vector<VkExtensionProperties> available(count);
    vkCheck(vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()), "Instance extensions");
    if (hasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (validation_) {
        vkCheck(vkEnumerateInstanceLayerProperties(&count, nullptr), "Layer count");
        std::vector<VkLayerProperties> layers(count);
        vkCheck(vkEnumerateInstanceLayerProperties(&count, layers.data()), "Layers");
        if (std::none_of(layers.begin(), layers.end(),
                         [=](const auto& item) { return std::strcmp(item.layerName, layer) == 0; }))
            throw std::runtime_error(
                "Debug requires the pinned VK_LAYER_KHRONOS_validation files next to ProtoEditor.exe");
        vkCheck(vkEnumerateInstanceExtensionProperties(layer, &count, nullptr), "Validation extension count");
        std::vector<VkExtensionProperties> layerExtensions(count);
        vkCheck(vkEnumerateInstanceExtensionProperties(layer, &count, layerExtensions.data()), "Validation extensions");
        if (!hasExtension(layerExtensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME))
            throw std::runtime_error("Validation synchronization configuration unavailable");
        extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Proto Engine";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "Proto Engine";
    app.engineVersion = app.applicationVersion;
    app.apiVersion = VK_API_VERSION_1_3;
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = debugMessage;
    debug.pUserData = &log_;
    const VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &sync;
    const bool debugAvailable = hasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (validation_ && !debugAvailable)
        throw std::runtime_error("Debug utils required for validation reporting");
    if (validation_)
        debug.pNext = &validation;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = validation_ ? 1u : 0u;
    info.ppEnabledLayerNames = validation_ ? &layer : nullptr;
    info.pNext = debugAvailable ? &debug : nullptr;
    vkCheck(vkCreateInstance(&info, nullptr, &context_.instance), "vkCreateInstance");
    volkLoadInstance(context_.instance);
    if (debugAvailable) {
        debug.pNext = nullptr;
        vkCheck(vkCreateDebugUtilsMessengerEXT(context_.instance, &debug, nullptr, &messenger_), "Debug messenger");
    }
    log_.write("INFO", validation_ ? "Vulkan 1.3; core + synchronization validation enabled"
                                   : "Vulkan 1.3; release validation disabled");
}
void VulkanRenderer::createDevice() {
    uint32_t count{};
    vkCheck(vkEnumeratePhysicalDevices(context_.instance, &count, nullptr), "GPU count");
    std::vector<VkPhysicalDevice> devices(count);
    vkCheck(vkEnumeratePhysicalDevices(context_.instance, &count, devices.data()), "GPUs");
    int bestScore = -1;
    for (const auto device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3)
            continue;
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        f12.pNext = &f13;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &f12;
        vkGetPhysicalDeviceFeatures2(device, &features);
        if (!f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore ||
            !features.features.imageCubeArray)
            continue;
        vkCheck(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr), "Device extension count");
        std::vector<VkExtensionProperties> extensions(count);
        vkCheck(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()), "Device extensions");
        if (!hasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            continue;
        VkFormatProperties format{};
        vkGetPhysicalDeviceFormatProperties(device, VK_FORMAT_D32_SFLOAT, &format);
        constexpr VkFormatFeatureFlags depthFeatures =
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((format.optimalTilingFeatures & depthFeatures) != depthFeatures)
            continue;
        vkGetPhysicalDeviceFormatProperties(device, VK_FORMAT_R16G16B16A16_SFLOAT, &format);
        constexpr VkFormatFeatureFlags hdrFeatures = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                                     VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                                     VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((format.optimalTilingFeatures & hdrFeatures) != hdrFeatures)
            continue;
        vkGetPhysicalDeviceFormatProperties(device, VK_FORMAT_R32G32B32A32_SFLOAT, &format);
        if ((format.optimalTilingFeatures & hdrFeatures) != hdrFeatures)
            continue;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
        for (uint32_t i = 0; i < count; ++i) {
            VkBool32 present{};
            vkCheck(vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &present), "Surface support");
            constexpr auto requiredQueues = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if (!present || (families[i].queueFlags & requiredQueues) != requiredQueues)
                continue;
            const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 10;
            if (score <= bestScore)
                continue;
            bestScore = score;
            context_.physicalDevice = device;
            context_.queueFamily = i;
            timestampBits_ = families[i].timestampValidBits;
            timestampPeriod_ = properties.limits.timestampPeriod;
            deviceName_ = properties.deviceName;
            memoryBudget_ = hasExtension(extensions, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        }
    }
    if (!context_.physicalDevice)
        throw std::runtime_error("No suitable Vulkan 1.3 GPU: dynamic rendering, synchronization2, timeline, cube "
                                 "arrays, D32 and Win32 presentation required");
    const float priority = 1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = context_.queueFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures base{};
    base.imageCubeArray = VK_TRUE;
    std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (memoryBudget_)
        extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &f12;
    info.pEnabledFeatures = &base;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    vkCheck(vkCreateDevice(context_.physicalDevice, &info, nullptr, &context_.device), "vkCreateDevice");
    volkLoadDevice(context_.device);
    vkGetDeviceQueue(context_.device, context_.queueFamily, 0, &context_.queue);
    log_.write("INFO", "GPU: " + deviceName_);
}
void VulkanRenderer::waitIdle() {
    vkCheck(vkDeviceWaitIdle(context_.device), "GPU idle");
    for (auto& frame : frames_)
        collectGpuTimestamp(frame);
    for (const auto& frame : frames_)
        if (frame.timestampPending)
            gpuTimestampDrainOmitted_ = true;
}

void VulkanRenderer::collectGpuTimestamp(Frame& frame) {
    if (!frame.timestampPending || !frame.queries)
        return;
    uint64_t stamps[2]{};
    const auto result = vkGetQueryPoolResults(context_.device, frame.queries, 0, 2, sizeof(stamps), stamps,
                                              sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (result == VK_NOT_READY)
        return;
    if (result != VK_SUCCESS) {
        frame.timestampPending = false;
        gpuTimestampDrainOmitted_ = true;
        return;
    }
    const auto mask = timestampBits_ == 64 ? UINT64_MAX : (uint64_t(1) << timestampBits_) - 1;
    const double duration = static_cast<double>((stamps[1] - stamps[0]) & mask) * timestampPeriod_ / 1e6;
    if (frame.timestampSerial) {
        // Frame slots complete out of serial order when waitIdle drains the
        // physical array. The pending flag already guarantees one collection
        // per query, so every serial must enter the benchmark queue. Keep the
        // HUD's latest GPU duration tied to the highest serial only.
        if (frame.timestampSerial >= completedTimestampSerial_) {
            completedTimestampSerial_ = frame.timestampSerial;
            gpuMs_ = duration;
        }
        if (gpuTimestampCollectionEnabled_) {
            constexpr size_t maxQueuedGpuSamples = 1024;
            if (completedGpuSamples_.size() < maxQueuedGpuSamples)
                completedGpuSamples_.push_back({frame.timestampSerial, duration});
            else
                ++droppedGpuTimestampSamples_;
        }
    }
    frame.timestampPending = false;
}

void VulkanRenderer::setGpuTimestampCollectionEnabled(bool enabled) {
    gpuTimestampCollectionEnabled_ = enabled;
    if (!enabled)
        completedGpuSamples_.clear();
}

std::vector<GpuTimestampSample> VulkanRenderer::consumeGpuTimestampSamples() {
    std::vector<GpuTimestampSample> result;
    result.swap(completedGpuSamples_);
    return result;
}
void VulkanRenderer::destroySwapchain() {
    for (const auto& image : swapImages_) {
        if (image.complete)
            vkDestroySemaphore(context_.device, image.complete, nullptr);
        if (image.view)
            vkDestroyImageView(context_.device, image.view, nullptr);
    }
    swapImages_.clear();
    if (swapchain_)
        vkDestroySwapchainKHR(context_.device, swapchain_, nullptr);
    swapchain_ = {};
}
void VulkanRenderer::createSwapchain() {
    waitIdle();
    VkSurfaceCapabilitiesKHR capabilities{};
    vkCheck(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context_.physicalDevice, surface_, &capabilities),
            "Surface capabilities");
    uint32_t count{};
    vkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(context_.physicalDevice, surface_, &count, nullptr),
            "Surface format count");
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkCheck(vkGetPhysicalDeviceSurfaceFormatsKHR(context_.physicalDevice, surface_, &count, formats.data()),
            "Surface formats");
    if (formats.empty())
        throw std::runtime_error("No presentation surface formats");
    auto selected = formats.front();
    for (const auto& format : formats)
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            selected = format;
    if (selected.format != VK_FORMAT_B8G8R8A8_UNORM && selected.format != VK_FORMAT_R8G8B8A8_UNORM &&
        selected.format != VK_FORMAT_B8G8R8A8_SRGB && selected.format != VK_FORMAT_R8G8B8A8_SRGB)
        throw std::runtime_error("M0 requires an RGBA8/BGRA8 presentation surface for readback");
    context_.surfaceFormat = selected.format;
    int width{}, height{};
    glfwGetFramebufferSize(window_, &width, &height);
    swapExtent_ = capabilities.currentExtent;
    if (swapExtent_.width == std::numeric_limits<uint32_t>::max()) {
        swapExtent_.width = std::clamp(static_cast<uint32_t>(std::max(width, 1)), capabilities.minImageExtent.width,
                                       capabilities.maxImageExtent.width);
        swapExtent_.height = std::clamp(static_cast<uint32_t>(std::max(height, 1)), capabilities.minImageExtent.height,
                                        capabilities.maxImageExtent.height);
    }
    uint32_t requestedImages = std::max(2u, capabilities.minImageCount + 1);
    if (capabilities.maxImageCount)
        requestedImages = std::min(requestedImages, capabilities.maxImageCount);
    if (requestedImages < 2)
        throw std::runtime_error("At least two swapchain images required");
    constexpr VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if ((capabilities.supportedUsageFlags & usage) != usage)
        throw std::runtime_error("Surface does not support M0 capture readback");
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    for (const auto candidate : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                 VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
        if (capabilities.supportedCompositeAlpha & candidate) {
            alpha = candidate;
            break;
        }
    uint32_t presentCount{};
    vkCheck(vkGetPhysicalDeviceSurfacePresentModesKHR(context_.physicalDevice, surface_, &presentCount, nullptr),
            "Present mode count");
    std::vector<VkPresentModeKHR> presentModes(presentCount);
    vkCheck(vkGetPhysicalDeviceSurfacePresentModesKHR(context_.physicalDevice, surface_, &presentCount,
                                                       presentModes.data()),
            "Present modes");
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vSync_) {
        if (std::find(presentModes.begin(), presentModes.end(), VK_PRESENT_MODE_MAILBOX_KHR) != presentModes.end())
            presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (std::find(presentModes.begin(), presentModes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != presentModes.end())
            presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = requestedImages;
    info.imageFormat = selected.format;
    info.imageColorSpace = selected.colorSpace;
    info.imageExtent = swapExtent_;
    info.imageArrayLayers = 1;
    info.imageUsage = usage;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain_;
    VkSwapchainKHR replacement{};
    vkCheck(vkCreateSwapchainKHR(context_.device, &info, nullptr, &replacement), "Swapchain");
    destroySwapchain();
    swapchain_ = replacement;
    presentMode_ = presentMode;
    vkCheck(vkGetSwapchainImagesKHR(context_.device, swapchain_, &count, nullptr), "Swapchain image count");
    std::vector<VkImage> images(count);
    vkCheck(vkGetSwapchainImagesKHR(context_.device, swapchain_, &count, images.data()), "Swapchain images");
    context_.imageCount = count;
    swapImages_.resize(count);
    for (size_t i = 0; i < images.size(); ++i) {
        auto& image = swapImages_[i];
        image.image = images[i];
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = selected.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCheck(vkCreateImageView(context_.device, &view, nullptr, &image.view), "Swapchain image view");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCheck(vkCreateSemaphore(context_.device, &semaphore, nullptr, &image.complete), "Present semaphore");
    }
    ++rebuilds_;
    swapchainDirty_ = false;
    log_.write("INFO", "Swapchain " + std::to_string(swapExtent_.width) + "x" + std::to_string(swapExtent_.height) +
                           "; " + std::to_string(static_cast<int>(presentMode_)) + "; " + std::to_string(count) +
                           " images");
}
void VulkanRenderer::createFrames() {
    for (auto& frame : frames_) {
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = context_.queueFamily;
        vkCheck(vkCreateCommandPool(context_.device, &pool, nullptr, &frame.pool), "Frame command pool");
        VkCommandBufferAllocateInfo cmd{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cmd.commandPool = frame.pool;
        cmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd.commandBufferCount = 1;
        vkCheck(vkAllocateCommandBuffers(context_.device, &cmd, &frame.command), "Frame command buffer");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCheck(vkCreateFence(context_.device, &fence, nullptr, &frame.fence), "Frame fence");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCheck(vkCreateSemaphore(context_.device, &semaphore, nullptr, &frame.acquired), "Acquire semaphore");
        if (timestampBits_) {
            VkQueryPoolCreateInfo queries{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queries.queryCount = 2;
            vkCheck(vkCreateQueryPool(context_.device, &queries, nullptr, &frame.queries), "GPU timestamp queries");
        }
    }
}
void VulkanRenderer::createPipeline(const std::filesystem::path& directory) {
    Shader vertex(context_.device, directory / "triangle.vert.spv"),
        fragment(context_.device, directory / "triangle.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex.handle;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment.handle;
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const VkDynamicState dynamicStates[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    vkCheck(vkCreatePipelineLayout(context_.device, &layout, nullptr, &pipelineLayout_), "Triangle pipeline layout");
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat;
    rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipelineLayout_;
    vkCheck(vkCreateGraphicsPipelines(context_.device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_),
            "Triangle pipeline");
}
void VulkanRenderer::destroyTarget(Target& target) {
    if (target.colorView)
        vkDestroyImageView(context_.device, target.colorView, nullptr);
    if (target.depthView)
        vkDestroyImageView(context_.device, target.depthView, nullptr);
    if (target.color)
        vmaDestroyImage(allocator_, target.color, target.colorMemory);
    if (target.depth)
        vmaDestroyImage(allocator_, target.depth, target.depthMemory);
    const auto generation = target.generation;
    target = {};
    target.generation = generation;
}
void VulkanRenderer::createTarget(Target& target, VkExtent2D extent) {
    destroyTarget(target);
    target.extent = extent;
    ++target.generation;
    const auto create = [&](VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage& image,
                            VmaAllocation& allocation, VkImageView& view) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {extent.width, extent.height, 1};
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        vkCheck(vmaCreateImage(allocator_, &info, &memory, &image, &allocation, nullptr), "Viewport image");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {aspect, 0, 1, 0, 1};
        vkCheck(vkCreateImageView(context_.device, &viewInfo, nullptr, &view), "Viewport image view");
    };
    create(VK_FORMAT_R8G8B8A8_UNORM,
           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
           VK_IMAGE_ASPECT_COLOR_BIT, target.color, target.colorMemory, target.colorView);
    create(VK_FORMAT_D32_SFLOAT,
           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
           VK_IMAGE_ASPECT_DEPTH_BIT, target.depth, target.depthMemory, target.depthView);
}
bool VulkanRenderer::prepare(VkExtent2D viewportSize) {
    if (fatal_)
        throw std::runtime_error(fatalReason_.empty() ? "Vulkan renderer is unusable" : fatalReason_);
    frameStats_.frameWaitMs = 0;
    int width{}, height{};
    glfwGetFramebufferSize(window_, &width, &height);
    if (width <= 0 || height <= 0 || glfwGetWindowAttrib(window_, GLFW_ICONIFIED))
        return false;
    auto& frame = frames_[frameIndex_];
    const auto frameWaitStart = Clock::now();
    vkCheck(vkWaitForFences(context_.device, 1, &frame.fence, VK_TRUE, UINT64_MAX), "Wait frame fence");
    frameStats_.frameWaitMs = milliseconds(frameWaitStart);
    collectGpuTimestamp(frame);
    if (swapchainDirty_ || static_cast<uint32_t>(width) != swapExtent_.width ||
        static_cast<uint32_t>(height) != swapExtent_.height)
        createSwapchain();
    viewportSize.width = std::clamp(viewportSize.width, 1u, 8192u);
    viewportSize.height = std::clamp(viewportSize.height, 1u, 8192u);
    // Both in-flight targets must exist before a pause can safely retain the
    // viewport texture while frameIndex_ advances. Initial allocation is also
    // allowed while paused; an existing paused target is never resized.
    for (auto& candidate : frames_)
        if (!candidate.target.color || !candidate.target.depth)
            createTarget(candidate.target, viewportSize);
    if (!scenePaused_ &&
        (frame.target.extent.width != viewportSize.width || frame.target.extent.height != viewportSize.height))
        createTarget(frame.target, viewportSize);
    return true;
}
void VulkanRenderer::markFatal(std::string reason) noexcept {
    if (!fatal_) {
        fatal_ = true;
        try {
            fatalReason_ = std::move(reason);
            log_.write("ERROR", "Vulkan renderer became unusable: " + fatalReason_);
        } catch (...) {
            fatalReason_ = "Vulkan recovery failed";
        }
    }
}
void VulkanRenderer::recoverAcquiredFrame(Frame& frame, SwapImage& swap, uint32_t imageIndex) {
    // vkAcquireNextImageKHR signals the acquire semaphore before readback
    // allocation and command recording. A failed frame must consume that
    // semaphore and present the acquired image back to the WSI; otherwise a
    // sequence of failed applies can exhaust the swapchain image pool.
    // Recovery is best-effort and marks the renderer fatal on any operation
    // that could leave a fence or image permanently outstanding.
    frame.submitted = false;
    const auto resetPool = vkResetCommandPool(context_.device, frame.pool, 0);
    if (resetPool != VK_SUCCESS) {
        markFatal("Failed to reset command pool after an aborted acquired frame (VkResult " +
                  std::to_string(static_cast<int>(resetPool)) + ')');
        return;
    }
    const auto previousLayout = swap.layout;
    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (previousLayout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        const auto beginResult = vkBeginCommandBuffer(frame.command, &begin);
        if (beginResult != VK_SUCCESS) {
            markFatal("Failed to begin swapchain recovery command buffer (VkResult " +
                      std::to_string(static_cast<int>(beginResult)) + ')');
            return;
        }
        barrier(frame.command, swap.image, VK_IMAGE_ASPECT_COLOR_BIT, previousLayout,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
        const auto endResult = vkEndCommandBuffer(frame.command);
        if (endResult != VK_SUCCESS) {
            markFatal("Failed to end swapchain recovery command buffer (VkResult " +
                      std::to_string(static_cast<int>(endResult)) + ')');
            return;
        }
        command.commandBuffer = frame.command;
    }
    const auto resetFence = vkResetFences(context_.device, 1, &frame.fence);
    if (resetFence != VK_SUCCESS) {
        markFatal("Failed to reset fence after an aborted acquired frame (VkResult " +
                  std::to_string(static_cast<int>(resetFence)) + ')');
        return;
    }
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = frame.acquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = swap.complete;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    if (previousLayout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command;
    }
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    const auto submitted = vkQueueSubmit2(context_.queue, 1, &submit, frame.fence);
    if (submitted != VK_SUCCESS) {
        markFatal("Failed to consume acquire semaphore after an aborted frame (VkResult " +
                  std::to_string(static_cast<int>(submitted)) + ')');
        return;
    }
    swap.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &swap.complete;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;
    const auto presented = vkQueuePresentKHR(context_.queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
        swapchainDirty_ = true;
    else if (presented != VK_SUCCESS)
        markFatal("Failed to release an acquired swapchain image after an aborted frame (VkResult " +
                  std::to_string(static_cast<int>(presented)) + ')');
}
void VulkanRenderer::label(VkCommandBuffer cmd, const char* name) {
    if (vkCmdBeginDebugUtilsLabelEXT) {
        VkDebugUtilsLabelEXT labelInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        labelInfo.pLabelName = name;
        labelInfo.color[0] = 0.3f;
        labelInfo.color[1] = 0.7f;
        labelInfo.color[2] = 0.9f;
        labelInfo.color[3] = 1;
        vkCmdBeginDebugUtilsLabelEXT(cmd, &labelInfo);
    }
}
void VulkanRenderer::recordTriangle(VkCommandBuffer cmd, Target& target, const RenderView* view) {
    label(cmd, view ? "Proto / scene primitives + instances" : "Proto / viewport triangle + depth + winding");
    barrier(cmd, target.color, VK_IMAGE_ASPECT_COLOR_BIT,
            target.rendered ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            target.rendered ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_NONE,
            target.rendered ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier(cmd, target.depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    const bool lit = view && view->lightingMode == LightingMode::Scene;
    if (lit) {
        assetRenderer_->renderLit(frameIndex_, cmd, *view, target.extent, target.color, target.colorView, target.depth,
                                  target.depthView);
        barrier(cmd, target.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    }
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target.colorView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = lit ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.025f, 0.035f, 0.055f, 1}};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = target.depthView;
    depth.imageLayout = lit ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = lit ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = lit ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1, 0};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, target.extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &rendering);
    VkViewport viewport{0,
                        static_cast<float>(target.extent.height),
                        static_cast<float>(target.extent.width),
                        -static_cast<float>(target.extent.height),
                        0,
                        1};
    VkRect2D scissor{{0, 0}, target.extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    if (view)
        recordScene(cmd, *view);
    else {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdDraw(cmd, 9, 1, 0, 0);
    }
    if (view && !lit)
        assetRenderer_->draw(frameIndex_, cmd, *view);
    vkCmdEndRendering(cmd);
    barrier(cmd, target.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    target.rendered = true;
    ++sceneRenderFrames_;
    if (vkCmdEndDebugUtilsLabelEXT)
        vkCmdEndDebugUtilsLabelEXT(cmd);
}
bool VulkanRenderer::draw(const std::function<void(VkCommandBuffer)>& drawUi, const std::filesystem::path& capture,
                          const RenderView* view) {
    if (fatal_)
        throw std::runtime_error(fatalReason_.empty() ? "Vulkan renderer is unusable" : fatalReason_);
    frameStats_.renderSubmitMs = 0;
    auto& frame = frames_[frameIndex_];
    auto& target = frame.target;
    uint32_t imageIndex{};
    const auto acquired =
        vkAcquireNextImageKHR(context_.device, swapchain_, UINT64_MAX, frame.acquired, VK_NULL_HANDLE, &imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchainDirty_ = true;
        return false;
    }
    if (acquired != VK_SUBOPTIMAL_KHR)
        vkCheck(acquired, "Acquire image");
    else
        swapchainDirty_ = true;
    auto& swap = swapImages_[imageIndex];
    AcquiredFrameRecovery recovery{*this, frame, target, swap, imageIndex, target.rendered, sceneRenderFrames_};
    std::unique_ptr<ReadbackBuffer> screenshot, colorReadback, depthReadback;
    struct PendingReadback {
        VkDevice device;
        VkFence fence;
        bool pending{};
        ~PendingReadback() {
            if (pending)
                vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        }
    } readbackGuard{context_.device, frame.fence};
    if (!capture.empty()) {
        screenshot =
            std::make_unique<ReadbackBuffer>(allocator_, VkDeviceSize(swapExtent_.width) * swapExtent_.height * 4);
        colorReadback =
            std::make_unique<ReadbackBuffer>(allocator_, VkDeviceSize(target.extent.width) * target.extent.height * 4);
        depthReadback = std::make_unique<ReadbackBuffer>(allocator_, VkDeviceSize(target.extent.width) *
                                                                         target.extent.height * sizeof(float));
    }
    vkCheck(vkResetCommandPool(context_.device, frame.pool, 0), "Reset command pool");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const auto cmd = frame.command;
    vkCheck(vkBeginCommandBuffer(cmd, &begin), "Begin frame");
    if (frame.queries) {
        vkCmdResetQueryPool(cmd, frame.queries, 0, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, frame.queries, 0);
    }
    // A paused scene still needs its first render on each in-flight target.
    // Once a target has a valid shader-readable image, retain it and continue
    // recording only the UI/presentation work below.
    const bool renderScene = !scenePaused_ || !target.rendered;
    if (renderScene) {
        if (view)
            assetRenderer_->upload(frameIndex_, cmd, *view);
        recordTriangle(cmd, target, view);
    }
    if (view && !assetRenderer_->error().empty())
        throw std::runtime_error("AssetRenderer frame preparation failed: " + assetRenderer_->error());
    label(cmd, "Proto / editor UI");
    barrier(cmd, swap.image, VK_IMAGE_ASPECT_COLOR_BIT, swap.layout,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = swap.view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue.color = {{0.06f, 0.065f, 0.08f, 1}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, swapExtent_};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    vkCmdBeginRendering(cmd, &rendering);
    drawUi(cmd);
    vkCmdEndRendering(cmd);
    if (vkCmdEndDebugUtilsLabelEXT)
        vkCmdEndDebugUtilsLabelEXT(cmd);
    if (frame.queries)
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, frame.queries, 1);
    if (screenshot) {
        label(cmd, "Proto / test readback");
        barrier(cmd, swap.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier(cmd, target.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier(cmd, target.depth, VK_IMAGE_ASPECT_DEPTH_BIT,
                view && view->lightingMode == LightingMode::Scene ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                                                                  : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
        copyImage(cmd, swap.image, VK_IMAGE_ASPECT_COLOR_BIT, swapExtent_, screenshot->buffer);
        copyImage(cmd, target.color, VK_IMAGE_ASPECT_COLOR_BIT, target.extent, colorReadback->buffer);
        copyImage(cmd, target.depth, VK_IMAGE_ASPECT_DEPTH_BIT, target.extent, depthReadback->buffer);
        barrier(cmd, target.color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        VkMemoryBarrier2 hostBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        hostBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        hostBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        hostBarrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        hostBarrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &hostBarrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
        if (vkCmdEndDebugUtilsLabelEXT)
            vkCmdEndDebugUtilsLabelEXT(cmd);
    }
    barrier(cmd, swap.image, VK_IMAGE_ASPECT_COLOR_BIT,
            screenshot ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            screenshot ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            screenshot ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
    vkCheck(vkEndCommandBuffer(cmd), "End frame");
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    // Wait before the timestamp interval so GPU timing excludes the WSI wait.
    // M0 has one submission; a later split can overlap offscreen work with acquire.
    wait.semaphore = frame.acquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    // Presentation completion semaphores belong to swapchain images. A frame
    // fence alone does not prove that the presentation engine consumed one.
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = swap.complete;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    command.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &command;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    const auto renderSubmitStart = Clock::now();
    vkCheck(vkResetFences(context_.device, 1, &frame.fence), "Reset frame fence");
    vkCheck(vkQueueSubmit2(context_.queue, 1, &submit, frame.fence), "Submit frame");
    if (frame.queries) {
        frame.timestampSerial = ++submittedTimestampSerial_;
        frame.timestampPending = true;
    }
    // The acquire semaphore is now consumed by the real submission. Any
    // later present/readback failure must leave the submitted fence intact.
    recovery.dismiss();
    swap.layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    readbackGuard.pending = screenshot != nullptr;
    frame.submitted = true;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &swap.complete;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;
    const auto result = vkQueuePresentKHR(context_.queue, &present);
    frameStats_.renderSubmitMs = milliseconds(renderSubmitStart);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        swapchainDirty_ = true;
    else if (result != VK_SUCCESS) {
        markFatal("Present frame failed (VkResult " + std::to_string(static_cast<int>(result)) + ')');
        vkCheck(result, "Present frame");
    }
    if (screenshot) {
        vkCheck(vkWaitForFences(context_.device, 1, &frame.fence, VK_TRUE, UINT64_MAX), "Readback fence");
        readbackGuard.pending = false;
        screenshot->invalidate();
        colorReadback->invalidate();
        depthReadback->invalidate();
        const size_t screenBytes = size_t(swapExtent_.width) * swapExtent_.height * 4;
        std::vector<uint8_t> rgba(screenBytes);
        std::memcpy(rgba.data(), screenshot->data, screenBytes);
        if (context_.surfaceFormat == VK_FORMAT_B8G8R8A8_UNORM || context_.surfaceFormat == VK_FORMAT_B8G8R8A8_SRGB)
            for (size_t i = 0; i < screenBytes; i += 4)
                std::swap(rgba[i], rgba[i + 2]);
        savePng(capture, swapExtent_.width, swapExtent_.height, rgba);
        const size_t pixels = size_t(target.extent.width) * target.extent.height;
        const std::span<const uint8_t> colorBytes(static_cast<const uint8_t*>(colorReadback->data), pixels * 4);
        const std::span<const float> depthValues(static_cast<const float*>(depthReadback->data), pixels);
        capturedViewportPixels_.assign(colorBytes.begin(), colorBytes.end());
        capturedViewportExtent_ = target.extent;
        auto viewportPath = capture;
        viewportPath.replace_filename(capture.stem().wstring() + L"-viewport.png");
        savePng(viewportPath, target.extent.width, target.extent.height, colorBytes);
        if (view) {
            sceneReadback_ = {};
            sceneReadback_.finiteDepth = sceneReadback_.matchesCpu = true;
            sceneReadback_.exactDepth = view->exactDepth;
            for (const auto value : depthValues) {
                if (!std::isfinite(value) || value < 0 || value > 1)
                    sceneReadback_.finiteDepth = false;
                if (value < 0.99999f)
                    ++sceneReadback_.geometryPixels;
            }
            for (const auto& probe : view->depthProbes) {
                const auto x = std::min(static_cast<uint32_t>(probe.u * static_cast<float>(target.extent.width)),
                                        target.extent.width - 1);
                const auto y = std::min(static_cast<uint32_t>(probe.v * static_cast<float>(target.extent.height)),
                                        target.extent.height - 1);
                const float actual = depthValues[size_t(y) * target.extent.width + x];
                if (std::abs(actual - probe.depth) > 0.00003f) {
                    sceneReadback_.matchesCpu = false;
                    log_.write("ERROR", "Scene depth probe expected=" + std::to_string(probe.depth) +
                                            " actual=" + std::to_string(actual));
                }
                ++sceneReadback_.depthSamples;
            }
            for (const auto& probe : view->colorProbes) {
                const auto x =
                    std::min(static_cast<uint32_t>(probe.u * float(target.extent.width)), target.extent.width - 1);
                const auto y =
                    std::min(static_cast<uint32_t>(probe.v * float(target.extent.height)), target.extent.height - 1);
                const size_t at = (size_t(y) * target.extent.width + x) * 4;
                const glm::vec3 actual(colorBytes[at], colorBytes[at + 1], colorBytes[at + 2]);
                const bool matches =
                    glm::all(glm::lessThanEqual(glm::abs(actual - probe.rgb), glm::vec3(probe.tolerance)));
                sceneReadback_.matchesCpu &= matches;
                ++sceneReadback_.colorSamples;
                log_.write(matches ? "TEST" : "ERROR",
                           "PBR probe " + probe.label + " actual=" + std::to_string(actual.r) + "," +
                               std::to_string(actual.g) + "," + std::to_string(actual.b) +
                               " expected=" + std::to_string(probe.rgb.r) + "," + std::to_string(probe.rgb.g) + "," +
                               std::to_string(probe.rgb.b));
            }
            log_.write(sceneReadback_.passed() ? "INFO" : "ERROR",
                       "Scene GPU readback: geometry_pixels=" + std::to_string(sceneReadback_.geometryPixels) +
                           " depth_probes=" + std::to_string(sceneReadback_.depthSamples));
        } else {
            pixelChecks_ = checkTriangle(target.extent.width, target.extent.height, colorBytes, depthValues);
            log_.write(pixelChecks_.passed() ? "INFO" : "ERROR",
                       "GPU readback: y_up=" + std::to_string(pixelChecks_.yUp) +
                           " front_face=" + std::to_string(pixelChecks_.frontFace) +
                           " back_culled=" + std::to_string(pixelChecks_.backFaceCulled) +
                           " depth=" + std::to_string(pixelChecks_.depthOcclusion) +
                           " clear_depth=" + std::to_string(pixelChecks_.clearDepth));
        }
    }
    ++renderedFrames_;
    frameIndex_ = (frameIndex_ + 1) % framesInFlight;
    return true;
}
size_t VulkanRenderer::allocatedBytes() const {
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(allocator_, &stats);
    return static_cast<size_t>(stats.total.statistics.allocationBytes);
}
VmaMemoryStats VulkanRenderer::memoryStats() {
    VmaMemoryStats result;
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(allocator_, &stats);
    result.allocationBytes = static_cast<uint64_t>(stats.total.statistics.allocationBytes);
    peakVmaAllocationBytes_ = std::max(peakVmaAllocationBytes_, result.allocationBytes);
    result.peakAllocationBytes = peakVmaAllocationBytes_;
    result.budgetAvailable = memoryBudget_;
    if (memoryBudget_) {
        std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
        vmaGetHeapBudgets(allocator_, budgets.data());
        for (const auto& budget : budgets) {
            result.heapUsageBytes += static_cast<uint64_t>(budget.usage);
            result.heapBudgetBytes += static_cast<uint64_t>(budget.budget);
        }
    }
    return result;
}
} // namespace proto
