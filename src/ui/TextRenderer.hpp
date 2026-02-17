#pragma once

#include "gfx/core/VulkanContext.hpp"
#include "gfx/rendering/Renderer.hpp"
#include "gfx/rendering/Pipeline.hpp"
#include "FontSDF.hpp"
#include <memory>

namespace ui {

struct TextVertex {
    float x, y;
    float u, v;
};

class TextRenderer {
public:
    TextRenderer(gfx::VulkanContext& context, gfx::Renderer& renderer);
    ~TextRenderer();

    void beginFrame(uint32_t currentFrame);

    void renderText(VkCommandBuffer commandBuffer, const std::string& text, float x, float y, float scale, float color[3]);

private:
    void createPipeline(const std::vector<VkFormat>& colorFormats, VkFormat depthFormat);

    gfx::VulkanContext& m_context;
    gfx::Renderer& m_renderer;

    std::unique_ptr<FontSDF> m_fontSDF;

    gfx::Pipeline* m_pipeline;
    
    // Per-frame vertex buffers
    std::vector<VkBuffer> m_vertexBuffers; 
    std::vector<VkDeviceMemory> m_vertexMemories;
    std::vector<void*> m_mappedDatas;
    
    size_t m_maxVertexCount = 4096; // 1024 quads per frame
    uint32_t m_currentFrame = 0;
    size_t m_currentVertexOffset = 0; // In vertices
};

} // namespace ui
