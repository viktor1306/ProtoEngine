#pragma once

#include "../gfx/VulkanContext.hpp"
#include "../gfx/BindlessSystem.hpp"
#include <string>
#include <vector>
#include <map>

namespace ui {

struct GlyphInfo {
    float u0, v0, u1, v1; // UV coordinates in atlas
    float width, height;  // Size in pixels (or relative)
    float bearingX, bearingY; // Offsets
    float advance; // Horizontal advance
};

class FontSDF {
public:
    FontSDF(gfx::VulkanContext& context, gfx::BindlessSystem& bindlessSystem, const std::string& fontPath);
    ~FontSDF();

    // Returns the texture ID in the global Bindless array
    uint32_t getTextureID() const { return m_textureID; }

    // Get glyph metrics for a character
    const GlyphInfo& getGlyphInfo(char c) const;

    float getScale() const { return m_scale; }

private:
    void generateAtlas(const std::string& fontPath);

    gfx::VulkanContext& m_context;
    gfx::BindlessSystem& m_bindlessSystem;

    VkImage m_image;
    VkDeviceMemory m_memory;
    VkImageView m_imageView;
    VkSampler m_sampler;
    uint32_t m_textureID;

    std::map<char, GlyphInfo> m_glyphs;
    float m_scale = 1.0f; // Scale factor used during generation
    float m_ascent = 0.0f;
    float m_descent = 0.0f;
    float m_lineGap = 0.0f;
};

} // namespace ui
