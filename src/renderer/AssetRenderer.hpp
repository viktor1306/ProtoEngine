#pragma once
#include "renderer/RenderView.hpp"
#include "renderer/VulkanRenderer.hpp"
namespace proto {
struct LightingStats {
    uint32_t lights{}, shadowFaces{}, shadowCacheHits{}, shadowMisses{}, batches{}, shadowSlots{}, tileOverflows{},
        drawCalls{};
    // Per-frame scene and pass counters used by the standalone benchmark.
    // drawCalls preserves the existing geometry-call meaning; totalDrawCalls
    // also includes the tone-map and legacy asset passes.
    uint64_t rawInstances{}, visibleInstances{}, culledInstances{};
    uint32_t depthDrawCalls{}, shadowDrawCalls{}, baseDrawCalls{}, lightingDrawCalls{}, toneMapDrawCalls{},
        legacyDrawCalls{}, totalDrawCalls{}, dispatchCount{};
    uint64_t assetUploadBytes{}, dynamicLightingUploadBytes{};
    double gatherMs{}, planMs{}, uploadFrameMs{};
    uint64_t shadowBytes{}, sunShadowBytes{}, pointShadowBytes{};
};
class AssetRenderer {
  public:
    AssetRenderer(const VulkanContext& context, VmaAllocator allocator, const std::filesystem::path& shaders);
    ~AssetRenderer();
    void upload(size_t frame, VkCommandBuffer command, const RenderView& view);
    void draw(size_t frame, VkCommandBuffer command, const RenderView& view);
    void renderLit(size_t frame, VkCommandBuffer command, const RenderView& view, VkExtent2D extent, VkImage color,
                   VkImageView colorView, VkImage depth, VkImageView depthView);
    // Discard CPU-side cache progress and per-frame lighting state when
    // command recording aborts before queue submission. The next frame will
    // rebuild resources from the immutable asset catalog.
    void abortFrame(size_t frame) noexcept;
    const LightingStats& lightingStats() const;
    size_t pending() const;
    size_t uploadedBytes() const;
    size_t meshCount() const;
    size_t imageCount() const;
    const std::string& error() const;
    void clearError();
    static void setAllocationFailureForTesting(bool enabled);
    static void setAllocationFailureAfterUploadForTesting(bool enabled);
    static void setAllocationFailureAfterLightingForTesting(bool enabled);

  private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace proto
