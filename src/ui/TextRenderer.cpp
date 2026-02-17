#include "TextRenderer.hpp"
#include "../core/Window.hpp"
#include <iostream>
#include <stdexcept>

// Use the canonical constant from Renderer to avoid magic numbers
static constexpr int MAX_FRAMES = gfx::Renderer::MAX_FRAMES_IN_FLIGHT;

namespace ui {

TextRenderer::TextRenderer(gfx::VulkanContext& context, gfx::Renderer& renderer)
    : m_context(context), m_renderer(renderer) {
    
    // Initialize FontSDF — try multiple paths to support running from project root or bin/
    static const char* fontPaths[] = {
        "bin/fonts/consola.ttf",   // run from project root
        "fonts/consola.ttf",       // run from bin/
        "../bin/fonts/consola.ttf" // run from obj/ or similar
    };
    bool fontLoaded = false;
    for (const char* path : fontPaths) {
        try {
            m_fontSDF = std::make_unique<FontSDF>(context, renderer.getBindlessSystem(), path);
            fontLoaded = true;
            break;
        } catch (...) {}
    }
    if (!fontLoaded) {
        throw std::runtime_error("Failed to load SDF Font from any known path (bin/fonts/consola.ttf)");
    }

    if (renderer.getSwapchain().getImages().size() > 0) {
         createPipeline({renderer.getSwapchain().getImageFormat()}, renderer.getSwapchain().getDepthFormat());
    } else {
        throw std::runtime_error("Swapchain images not ready when creating TextRenderer!");
    }
    
    // Create persistent vertex buffers for each frame in flight
    m_vertexBuffers.resize(MAX_FRAMES);
    m_vertexMemories.resize(MAX_FRAMES);
    m_mappedDatas.resize(MAX_FRAMES);
    
    VkDeviceSize bufferSize = m_maxVertexCount * sizeof(TextVertex);
    
    for (int i = 0; i < MAX_FRAMES; i++) {
        m_context.createBuffer(bufferSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, m_vertexBuffers[i], m_vertexMemories[i]);
        vkMapMemory(m_context.getDevice(), m_vertexMemories[i], 0, bufferSize, 0, &m_mappedDatas[i]);
    }
}

TextRenderer::~TextRenderer() {
    VkDevice device = m_context.getDevice();
    
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (m_mappedDatas[i]) {
            vkUnmapMemory(device, m_vertexMemories[i]);
        }
        vkDestroyBuffer(device, m_vertexBuffers[i], nullptr);
        vkFreeMemory(device, m_vertexMemories[i], nullptr);
    }

    m_fontSDF.reset(); // Destroy FontSDF (release image/sampler/bindless ID)
    
    delete m_pipeline;
}

void TextRenderer::createPipeline(const std::vector<VkFormat>& colorFormats, VkFormat depthFormat) {
    gfx::PipelineConfig config{};
    config.colorAttachmentFormats = colorFormats;
    config.depthAttachmentFormat = depthFormat;
    
    // Explicit Vertex Input for Text
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(TextVertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    config.bindingDescriptions.push_back(bindingDescription);
    
    // Location 0: Pos (vec2)
    VkVertexInputAttributeDescription posAttribute{};
    posAttribute.binding = 0;
    posAttribute.location = 0;
    posAttribute.format = VK_FORMAT_R32G32_SFLOAT;
    posAttribute.offset = offsetof(TextVertex, x);
    config.attributeDescriptions.push_back(posAttribute);

    // Location 1: UV (vec2)
    VkVertexInputAttributeDescription uvAttribute{};
    uvAttribute.binding = 0;
    uvAttribute.location = 1;
    uvAttribute.format = VK_FORMAT_R32G32_SFLOAT;
    uvAttribute.offset = offsetof(TextVertex, u);
    config.attributeDescriptions.push_back(uvAttribute);

    config.vertexShaderPath = "bin/shaders/text.vert.spv";
    config.fragmentShaderPath = "bin/shaders/text.frag.spv";
    config.enableDepthTest = false; // Disable depth for UI
    config.enableBlend = true; // Enable blending for alpha
    config.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    config.cullMode = VK_CULL_MODE_NONE;
    
    // USE BINDLESS LAYOUT (Set 0 in shader = Set 1 in BindlessSystem logic? 
    // Wait. In text.frag: layout(set = 0, binding = 0) uniform sampler2D textures[];
    // BindlessSystem::getDescriptorSetLayout() corresponds to the layout that has binding 0 as textures[].
    // So we just need to push that layout.
    
    config.descriptorSetLayouts.push_back(m_renderer.getBindlessSystem().getDescriptorSetLayout());
    
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT; 
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(float) * 7 + sizeof(uint32_t); // vec2+vec2+vec3 + uint
    config.pushConstantRanges.push_back(pushConstantRange);

    m_pipeline = new gfx::Pipeline(m_context, config);
}

void TextRenderer::beginFrame(uint32_t currentFrame) {
    m_currentFrame = currentFrame % MAX_FRAMES;
    m_currentVertexOffset = 0;
}

void TextRenderer::renderText(VkCommandBuffer commandBuffer, const std::string& text, float x, float y, float scale, float color[3]) {
    m_pipeline->bind(commandBuffer);
    
    // Bind Bindless Descriptor Set (Set 0 in shader, Set 1 in engine usually?)
    // In shader I wrote: layout(set = 0, binding = 0) uniform sampler2D textures[];
    // So we bind it to set 0.
    // BindlessSystem::bind binds to set 1 by default.
    // Let's bind it manually to set 0 since this pipeline is custom.
    
    m_renderer.getBindlessSystem().bind(commandBuffer, m_pipeline->getLayout(), m_currentFrame, 0); 
    
    struct PushConst {
        float scale[2];
        float translate[2];
        float color[3];
        uint32_t textureID;
    } pc;
    
    pc.scale[0] = 1.0f; pc.scale[1] = 1.0f; // Simplified, assuming gl_Position logic in shader matches what we want
    // text.vert: gl_Position = vec4(inPosition * pc.scale + pc.translate, 0.0, 1.0);
    // x,y are in clip space [-1, 1]? Or screen space?
    // Old implementation passed vertices in mostly clip space logic?
    // "start x = x". If user passes -0.5, it is left.
    // Let's keep 1.0 scale for now and let x/y be strictly coords.
    
    pc.translate[0] = 0.0f; pc.translate[1] = 0.0f;
    pc.color[0] = color[0]; pc.color[1] = color[1]; pc.color[2] = color[2];
    pc.textureID = m_fontSDF->getTextureID();
    
    vkCmdPushConstants(commandBuffer, m_pipeline->getLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConst), &pc); 
    
    // vkCmdSetDepthBias(commandBuffer, 0.0f, 0.0f, 0.0f); // Removed as dynamic state is disabled
    
    std::vector<TextVertex> vertices;
    
    float startX = x;
    float startY = y; 
    
    // Scale adjustment for SDF. 
    // m_fontSDF->getScale() returns scale for 32px.
    // We want to render at 'scale' size (e.g. 0.05 in clip space).
    // The vertices should be generated relative to startX/startY.
    
    // We normalize metrics to be roughly 1.0 height = 'scale'.
    // metrics from stbtt are quite large (e.g. advance ~20).
    // 32px height at 512 texture. 
    // We can just use the pixel sizes from FontSDF and multiply by 'scale' * constant (e.g. 0.001) to get to clip space.
    // Or simpler:
    float fontScale = scale * 0.05f; // Magic number to tune size

    for (char c : text) {
        const GlyphInfo& glyph = m_fontSDF->getGlyphInfo(c);
        
        float x0 = startX + glyph.bearingX * fontScale;
        float y0 = startY + glyph.bearingY * fontScale;
        float x1 = x0 + glyph.width * fontScale;
        float y1 = y0 + glyph.height * fontScale;
        
        float u0 = glyph.u0;
        float v0 = glyph.v0;
        float u1 = glyph.u1;
        float v1 = glyph.v1;
        
        // Quad
        vertices.push_back({x0, y0, u0, v0});
        vertices.push_back({x1, y0, u1, v0});
        vertices.push_back({x0, y1, u0, v1});
        
        vertices.push_back({x1, y0, u1, v0});
        vertices.push_back({x1, y1, u1, v1});
        vertices.push_back({x0, y1, u0, v1});
        
        startX += glyph.advance * fontScale * 0.6f; // Tighter spacing
    }
    
    size_t dataSize = vertices.size() * sizeof(TextVertex);
    if (dataSize == 0) return;
    
    if (m_currentVertexOffset + vertices.size() > m_maxVertexCount) {
        return; 
    }
    
    uint8_t* mappedBytes = static_cast<uint8_t*>(m_mappedDatas[m_currentFrame]);
    mappedBytes += m_currentVertexOffset * sizeof(TextVertex);
    memcpy(mappedBytes, vertices.data(), dataSize);
    
    VkDeviceSize offsets[] = {m_currentVertexOffset * sizeof(TextVertex)};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &m_vertexBuffers[m_currentFrame], offsets);
    
    vkCmdDraw(commandBuffer, static_cast<uint32_t>(vertices.size()), 1, 0, 0);
    
    m_currentVertexOffset += vertices.size();
}

} // namespace ui
