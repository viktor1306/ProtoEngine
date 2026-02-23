#pragma once

#include "gfx/core/VulkanContext.hpp"
#include "gfx/rendering/Pipeline.hpp"
#include "gfx/resources/Buffer.hpp"
#include "core/Math.hpp"
#include <vulkan/vulkan.h>
#include <vector>
#include <memory>

namespace gfx {

// ---------------------------------------------------------------------------
// DebugRenderer — simple line-list renderer for frustum / camera markers.
//
// Usage (each frame):
//   debugRenderer.begin();
//   debugRenderer.addLine(from, to, {r,g,b});
//   debugRenderer.addFrustum(invViewProj, {r,g,b});
//   debugRenderer.draw(cmd, pipelineLayout, viewProj);
// ---------------------------------------------------------------------------
class DebugRenderer {
public:
    static constexpr uint32_t MAX_VERTS = 4096;

    struct Vertex {
        float x, y, z;
        float r, g, b;
    };

    struct DebugPush {
        core::math::Mat4 viewProj;
    };

    DebugRenderer(VulkanContext& ctx,
                  VkRenderingInfo* /*unused*/,
                  VkFormat colorFmt, VkFormat depthFmt,
                  const std::string& vertSpvPath,
                  const std::string& fragSpvPath);
    ~DebugRenderer() = default;

    DebugRenderer(const DebugRenderer&) = delete;
    DebugRenderer& operator=(const DebugRenderer&) = delete;

    // Reset vertex list for this frame
    void begin();

    // Add a single coloured line segment
    void addLine(core::math::Vec3 a, core::math::Vec3 b, core::math::Vec3 color);

    // Draw all 12 edges of the given frustum.
    // invViewProj — inverse of (proj * view) of the camera to visualise.
    void addFrustum(const core::math::Mat4& invViewProj, core::math::Vec3 color);

    // Small XYZ axis cross at `center` (half-length = size)
    void addCross(core::math::Vec3 center, float size);

    // Small camera-widget: a cone-like pyramid visible from any angle.
    void addCameraMarker(const core::math::Mat4& invViewProj, core::math::Vec3 color);

    // Upload CPU vertices, bind pipeline and draw
    void draw(VkCommandBuffer cmd, const core::math::Mat4& viewProj);

    VkPipelineLayout getLayout() const { return m_pipeline->getLayout(); }

private:
    core::math::Vec3 unproject(const core::math::Mat4& invVP, float ndcX, float ndcY, float ndcZ);

    VulkanContext& m_ctx;
    std::unique_ptr<Pipeline> m_pipeline;
    std::unique_ptr<Buffer>   m_vertexBuffer;

    std::vector<Vertex> m_verts;
};

} // namespace gfx
