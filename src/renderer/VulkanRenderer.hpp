#pragma once
#include "core/Diagnostics.hpp"
#include "core/Image.hpp"
#include "renderer/RenderView.hpp"
#include "renderer/PrimitiveGeometry.hpp"
#include <volk.h>
#include <vk_mem_alloc.h>
#include <array>
#include <functional>
#include <vector>

struct GLFWwindow;
namespace proto {
class AssetRenderer;
void vkCheck(VkResult result, const char* operation);
struct VulkanContext {
    VkInstance instance{};
    VkPhysicalDevice physicalDevice{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t queueFamily{};
    VkFormat surfaceFormat{};
    uint32_t imageCount{};
};
struct GpuTimestampSample {
    uint64_t serial{};
    double milliseconds{};
};
struct RendererFrameStats {
    double frameWaitMs{};
    double renderSubmitMs{};
};
struct VmaMemoryStats {
    bool budgetAvailable{};
    uint64_t allocationBytes{};
    uint64_t peakAllocationBytes{};
    uint64_t heapUsageBytes{};
    uint64_t heapBudgetBytes{};
};
class VulkanRenderer {
  public:
    static constexpr size_t framesInFlight = 2;
    explicit VulkanRenderer(Diagnostics& log);
    ~VulkanRenderer();
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;
    void initialize(GLFWwindow* window, const std::filesystem::path& shaderDirectory, bool validation);
    bool prepare(VkExtent2D viewportSize);
    bool draw(const std::function<void(VkCommandBuffer)>& drawUi, const std::filesystem::path& capture = {},
              const RenderView* view = nullptr);
    void waitIdle();
    void setVSync(bool enabled) {
        if (vSync_ != enabled) {
            vSync_ = enabled;
            swapchainDirty_ = true;
        }
    }
    bool vSync() const { return vSync_; }
    VkPresentModeKHR presentMode() const { return presentMode_; }
    // Freeze the offscreen scene texture once each in-flight target has been
    // rendered. Presentation and UI recording continue while paused.
    void setScenePaused(bool paused) { scenePaused_ = paused; }
    bool scenePaused() const { return scenePaused_; }
    const VulkanContext& context() const { return context_; }
    size_t frameIndex() const { return frameIndex_; }
    VkImageView viewportView() const { return frames_[frameIndex_].target.colorView; }
    uint64_t viewportGeneration() const { return frames_[frameIndex_].target.generation; }
    VkExtent2D viewportExtent() const { return frames_[frameIndex_].target.extent; }
    VkExtent2D swapExtent() const { return swapExtent_; }
    const std::string& deviceName() const { return deviceName_; }
    const PixelChecks& pixelChecks() const { return pixelChecks_; }
    const SceneReadback& sceneReadback() const { return sceneReadback_; }
    const std::vector<uint8_t>& capturedViewportPixels() const { return capturedViewportPixels_; }
    VkExtent2D capturedViewportExtent() const { return capturedViewportExtent_; }
    double gpuMilliseconds() const { return gpuMs_; }
    bool hasGpuTiming() const { return timestampBits_ != 0; }
    RendererFrameStats frameStats() const { return frameStats_; }
    uint64_t submittedGpuTimestampSerial() const { return submittedTimestampSerial_; }
    // Regular Player/Editor runs only need gpuMilliseconds(); benchmark runs
    // opt into the consumable serial queue explicitly to avoid retaining a
    // timestamp sample for every frame of a long-lived process.
    void setGpuTimestampCollectionEnabled(bool enabled);
    bool gpuTimestampCollectionEnabled() const { return gpuTimestampCollectionEnabled_; }
    std::vector<GpuTimestampSample> consumeGpuTimestampSamples();
    bool gpuTimestampDrainOmitted() const { return gpuTimestampDrainOmitted_; }
    uint64_t droppedGpuTimestampSamples() const { return droppedGpuTimestampSamples_; }
    uint32_t swapchainRebuilds() const { return rebuilds_; }
    bool validationEnabled() const { return validation_; }
    bool fatal() const { return fatal_; }
    const std::string& fatalReason() const { return fatalReason_; }
    uint64_t renderedFrames() const { return renderedFrames_; }
    uint64_t sceneRenderFrames() const { return sceneRenderFrames_; }
    size_t allocatedBytes() const;
    VmaMemoryStats memoryStats();
    AssetRenderer* assetRenderer() const { return assetRenderer_.get(); }

  private:
    struct Target {
        VkImage color{}, depth{};
        VmaAllocation colorMemory{}, depthMemory{};
        VkImageView colorView{}, depthView{};
        VkExtent2D extent{};
        uint64_t generation{};
        bool rendered{};
    };
    struct Frame {
        VkCommandPool pool{};
        VkCommandBuffer command{};
        VkFence fence{};
        VkSemaphore acquired{};
        VkQueryPool queries{};
        bool submitted{};
        VkBuffer instances{};
        VmaAllocation instanceMemory{};
        void* instanceData{};
        size_t instanceCapacity{};
        uint64_t timestampSerial{};
        bool timestampPending{};
        Target target;
    };
    struct SwapImage {
        VkImage image{};
        VkImageView view{};
        VkSemaphore complete{};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    };
    struct AcquiredFrameRecovery {
        VulkanRenderer& renderer;
        Frame& frame;
        Target& target;
        SwapImage& swap;
        uint32_t imageIndex{};
        bool targetRenderedBefore{};
        uint64_t sceneRenderFramesBefore{};
        bool active{true};
        ~AcquiredFrameRecovery() noexcept;
        void dismiss() noexcept { active = false; }
    };
    void createInstance();
    void createDevice();
    void createSwapchain();
    void destroySwapchain();
    void createFrames();
    void createPipeline(const std::filesystem::path& directory);
    void createScenePipelines(const std::filesystem::path& directory);
    void createGeometry();
    void recordScene(VkCommandBuffer cmd, const RenderView& view);
    void createTarget(Target& target, VkExtent2D extent);
    void destroyTarget(Target& target);
    // vkAcquireNextImageKHR signals Frame::acquired before any of the
    // recording/allocation work below can fail. Consume that semaphore with a
    // tiny recovery submission so a failed frame cannot poison the next
    // acquire. This helper is best-effort and never throws from stack
    // unwinding.
    void recoverAcquiredFrame(Frame& frame, SwapImage& swap, uint32_t imageIndex);
    void collectGpuTimestamp(Frame& frame);
    void markFatal(std::string reason) noexcept;
    void recordTriangle(VkCommandBuffer cmd, Target& target, const RenderView* view);
    void label(VkCommandBuffer cmd, const char* name);
    Diagnostics& log_;
    GLFWwindow* window_{};
    VulkanContext context_;
    VkDebugUtilsMessengerEXT messenger_{};
    VkSurfaceKHR surface_{};
    VkSwapchainKHR swapchain_{};
    VkExtent2D swapExtent_{};
    VkPresentModeKHR presentMode_{VK_PRESENT_MODE_FIFO_KHR};
    VmaAllocator allocator_{};
    VkPipelineLayout pipelineLayout_{};
    VkPipeline pipeline_{};
    VkPipelineLayout sceneLayout_{};
    std::array<VkPipeline, 3> scenePipelines_{}; // CCW, mirrored CW, editor lines
    VkBuffer geometry_{};
    VmaAllocation geometryMemory_{};
    VkDeviceSize indexOffset_{};
    std::array<MeshRange, 4> meshRanges_{};
    struct SceneInstance {
        glm::mat4 world;
        glm::vec4 color;
    };
    std::array<std::vector<SceneInstance>, 6> instanceGroups_;
    std::vector<SwapImage> swapImages_;
    std::array<Frame, framesInFlight> frames_{};
    size_t frameIndex_{};
    uint32_t timestampBits_{};
    float timestampPeriod_{};
    double gpuMs_{};
    RendererFrameStats frameStats_{};
    uint64_t submittedTimestampSerial_{};
    uint64_t completedTimestampSerial_{};
    std::vector<GpuTimestampSample> completedGpuSamples_;
    bool gpuTimestampCollectionEnabled_{};
    uint64_t droppedGpuTimestampSamples_{};
    bool gpuTimestampDrainOmitted_{};
    uint64_t peakVmaAllocationBytes_{};
    uint64_t renderedFrames_{};
    uint64_t sceneRenderFrames_{};
    uint32_t rebuilds_{};
    bool validation_{};
    bool swapchainDirty_{};
    bool vSync_{true};
    bool scenePaused_{};
    bool memoryBudget_{};
    bool fatal_{};
    std::string fatalReason_;
    std::string deviceName_;
    PixelChecks pixelChecks_;
    SceneReadback sceneReadback_;
    std::vector<uint8_t> capturedViewportPixels_;
    VkExtent2D capturedViewportExtent_{};
    std::unique_ptr<AssetRenderer> assetRenderer_;
};
} // namespace proto
