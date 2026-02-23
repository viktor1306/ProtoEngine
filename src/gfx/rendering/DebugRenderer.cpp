#include "DebugRenderer.hpp"
#include <cstring>
#include <cassert>
#include <filesystem>

namespace gfx {

// ---------------------------------------------------------------------------
// Helper: unproject an NDC point using inverse view-projection
// ---------------------------------------------------------------------------
core::math::Vec3 DebugRenderer::unproject(const core::math::Mat4& invVP,
                                           float ndcX, float ndcY, float ndcZ)
{
    // invVP.data is column-major: data[col][row]
    float x = invVP.data[0][0]*ndcX + invVP.data[1][0]*ndcY + invVP.data[2][0]*ndcZ + invVP.data[3][0];
    float y = invVP.data[0][1]*ndcX + invVP.data[1][1]*ndcY + invVP.data[2][1]*ndcZ + invVP.data[3][1];
    float z = invVP.data[0][2]*ndcX + invVP.data[1][2]*ndcY + invVP.data[2][2]*ndcZ + invVP.data[3][2];
    float w = invVP.data[0][3]*ndcX + invVP.data[1][3]*ndcY + invVP.data[2][3]*ndcZ + invVP.data[3][3];
    if (std::abs(w) > 1e-7f) { x /= w; y /= w; z /= w; }
    return {x, y, z};
}

// ---------------------------------------------------------------------------
// DebugRenderer
// ---------------------------------------------------------------------------
DebugRenderer::DebugRenderer(VulkanContext& ctx, VkRenderingInfo* /*unused*/,
                             VkFormat colorFmt, VkFormat depthFmt,
                             const std::string& vertSpvPath,
                             const std::string& fragSpvPath)
    : m_ctx(ctx)
{
    // ---- vertex buffer (host-visible, updated every frame) ----
    m_vertexBuffer = std::make_unique<Buffer>(
        ctx,
        sizeof(Vertex) * MAX_VERTS,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU,
        VMA_ALLOCATION_CREATE_MAPPED_BIT
    );

    // ---- push-constant range ----
    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(DebugPush);

    // ---- vertex layout: vec3 pos + vec3 color ----
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attrs(2);
    attrs[0].binding  = 0; attrs[0].location = 0;
    attrs[0].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset   = offsetof(Vertex, x);
    attrs[1].binding  = 0; attrs[1].location = 1;
    attrs[1].format   = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset   = offsetof(Vertex, r);

    PipelineConfig cfg{};
    cfg.colorAttachmentFormats = {colorFmt};
    cfg.depthAttachmentFormat  = depthFmt;
    cfg.vertexShaderPath       = vertSpvPath;
    cfg.fragmentShaderPath     = fragSpvPath;
    cfg.topology               = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    cfg.polygonMode            = VK_POLYGON_MODE_FILL;
    cfg.cullMode               = VK_CULL_MODE_NONE;
    cfg.enableDepthTest        = true;
    cfg.pushConstantRanges     = {pcRange};
    cfg.bindingDescriptions    = {binding};
    cfg.attributeDescriptions  = attrs;

    m_pipeline = std::make_unique<Pipeline>(ctx, cfg);
    m_verts.reserve(MAX_VERTS);
}

void DebugRenderer::begin() {
    m_verts.clear();
}

void DebugRenderer::addLine(core::math::Vec3 a, core::math::Vec3 b, core::math::Vec3 col) {
    if (m_verts.size() + 2 > MAX_VERTS) return;
    m_verts.push_back({a.x, a.y, a.z, col.x, col.y, col.z});
    m_verts.push_back({b.x, b.y, b.z, col.x, col.y, col.z});
}

void DebugRenderer::addFrustum(const core::math::Mat4& invVP, core::math::Vec3 col) {
    // 8 corners: near/far planes x 4 corners
    // NDC z=0 = near, z=1 = far (Vulkan)
    core::math::Vec3 corners[8] = {
        unproject(invVP, -1,-1, 0), // near BL
        unproject(invVP,  1,-1, 0), // near BR
        unproject(invVP,  1, 1, 0), // near TR
        unproject(invVP, -1, 1, 0), // near TL
        unproject(invVP, -1,-1, 1), // far BL
        unproject(invVP,  1,-1, 1), // far BR
        unproject(invVP,  1, 1, 1), // far TR
        unproject(invVP, -1, 1, 1), // far TL
    };

    // Near plane edges (0-3)
    addLine(corners[0], corners[1], col);
    addLine(corners[1], corners[2], col);
    addLine(corners[2], corners[3], col);
    addLine(corners[3], corners[0], col);

    // Far plane edges (4-7)
    addLine(corners[4], corners[5], col);
    addLine(corners[5], corners[6], col);
    addLine(corners[6], corners[7], col);
    addLine(corners[7], corners[4], col);

    // Connecting edges
    addLine(corners[0], corners[4], col);
    addLine(corners[1], corners[5], col);
    addLine(corners[2], corners[6], col);
    addLine(corners[3], corners[7], col);
}

void DebugRenderer::addCross(core::math::Vec3 c, float sz) {
    addLine({c.x-sz, c.y, c.z}, {c.x+sz, c.y, c.z}, {1,0,0});
    addLine({c.x, c.y-sz, c.z}, {c.x, c.y+sz, c.z}, {0,1,0});
    addLine({c.x, c.y, c.z-sz}, {c.x, c.y, c.z+sz}, {0,0,1});
}

void DebugRenderer::addCameraMarker(const core::math::Mat4& invVP, core::math::Vec3 col) {
    // Camera "eye" point (unproject the NDC origin at near plane)
    core::math::Vec3 eye  = unproject(invVP, 0, 0, 0);
    core::math::Vec3 nCorners[4] = {
        unproject(invVP, -0.15f, -0.15f, 0),
        unproject(invVP,  0.15f, -0.15f, 0),
        unproject(invVP,  0.15f,  0.15f, 0),
        unproject(invVP, -0.15f,  0.15f, 0),
    };

    // Small cross at eye position
    float sz = 0.5f;
    addCross(eye, sz);

    // Lines from eye to near-plane corners
    for (int i = 0; i < 4; ++i)
        addLine(eye, nCorners[i], col);
    // Near rectangle
    for (int i = 0; i < 4; ++i)
        addLine(nCorners[i], nCorners[(i+1)%4], col);
}

void DebugRenderer::draw(VkCommandBuffer cmd, const core::math::Mat4& viewProj) {
    if (m_verts.empty()) return;

    // Upload vertices
    m_vertexBuffer->upload(m_verts.data(), m_verts.size() * sizeof(Vertex));

    // Bind pipeline
    m_pipeline->bind(cmd);

    // Push view-proj
    DebugPush push{viewProj};
    vkCmdPushConstants(cmd, m_pipeline->getLayout(),
                       VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(DebugPush), &push);

    // Bind vertex buffer
    VkBuffer buf = m_vertexBuffer->getBuffer();
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, offsets);

    vkCmdDraw(cmd, static_cast<uint32_t>(m_verts.size()), 1, 0, 0);
}

} // namespace gfx
