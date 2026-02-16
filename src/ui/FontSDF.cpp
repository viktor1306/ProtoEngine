#include "FontSDF.hpp"
#include <iostream>
#include <fstream>
#include <stdexcept>

// Define STB_TRUETYPE_IMPLEMENTATION in one source file
#define STB_TRUETYPE_IMPLEMENTATION
#include "../vendor/stb_truetype.h"

namespace ui {

FontSDF::FontSDF(gfx::VulkanContext& context, gfx::BindlessSystem& bindlessSystem, const std::string& fontPath)
    : m_context(context), m_bindlessSystem(bindlessSystem) {
    generateAtlas(fontPath);
}

FontSDF::~FontSDF() {
    VkDevice device = m_context.getDevice();
    
    // Unregister from Bindless System
    m_bindlessSystem.unregisterTexture(m_textureID);

    vkDestroySampler(device, m_sampler, nullptr);
    vkDestroyImageView(device, m_imageView, nullptr);
    vkDestroyImage(device, m_image, nullptr);
    vkFreeMemory(device, m_memory, nullptr);
}

const GlyphInfo& FontSDF::getGlyphInfo(char c) const {
    auto it = m_glyphs.find(c);
    if (it != m_glyphs.end()) {
        return it->second;
    }
    // Return space or fallback if not found?
    static GlyphInfo fallback{};
    return fallback;
}

void FontSDF::generateAtlas(const std::string& fontPath) {
    // 1. Load File
    std::ifstream file(fontPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open font file: " + fontPath);
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<unsigned char> fontBuffer(size);
    if (!file.read(reinterpret_cast<char*>(fontBuffer.data()), size)) {
         throw std::runtime_error("Failed to read font file: " + fontPath);
    }

    // 2. Init STBTT
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, fontBuffer.data(), 0)) {
        throw std::runtime_error("Failed to init font info");
    }

    // 3. Setup Atlas
    const int width = 512;
    const int height = 512;
    std::vector<unsigned char> bitmap(width * height, 0);

    const int padding = 2; // pixel padding around glyph
    const int onedge_value = 128; // value at edge
    const float pixel_dist_scale = 32.0f; // value scaling (higher = sharper falloff)

    float scale = stbtt_ScaleForPixelHeight(&info, 32.0f); // Target 32px height
    m_scale = scale;

    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
    m_ascent = ascent * scale;
    m_descent = descent * scale;
    m_lineGap = lineGap * scale;

    // 4. Generate Glyphs
    int x = 1;
    int y = 1;
    int bottomY = 1;

    for (int c = 32; c < 127; ++c) {
        int glyphIndex = stbtt_FindGlyphIndex(&info, c);
        if (glyphIndex == 0) continue;

        int advance, lsb;
        stbtt_GetGlyphHMetrics(&info, glyphIndex, &advance, &lsb);

        int bboxX1, bboxY1, bboxX2, bboxY2;
        stbtt_GetGlyphBitmapBox(&info, glyphIndex, scale, scale, &bboxX1, &bboxY1, &bboxX2, &bboxY2);
        
        int gw = bboxX2 - bboxX1;
        int gh = bboxY2 - bboxY1;
        
        // SDF padding
        int sdfPadding = 4; 
        int outW = gw + 2 * sdfPadding;
        int outH = gh + 2 * sdfPadding;

        if (x + outW >= width) {
            x = 1;
            y = bottomY + 1;
        }
        if (y + outH >= height) {
            std::cerr << "Font Atlas full!" << std::endl;
            break;
        }

        // Generate SDF
        // Note: stbtt_GetGlyphSDF allocates memory, we must free it.
        int sdfW, sdfH, xoff, yoff;
        unsigned char* sdf = stbtt_GetGlyphSDF(&info, scale, glyphIndex, sdfPadding, onedge_value, pixel_dist_scale, &sdfW, &sdfH, &xoff, &yoff);
        
        if (sdf) {
            for (int r = 0; r < sdfH; ++r) {
                for (int c = 0; c < sdfW; ++c) {
                    if (y + r < height && x + c < width) {
                        bitmap[(y + r) * width + (x + c)] = sdf[r * sdfW + c];
                    }
                }
            }
            stbtt_FreeSDF(sdf, nullptr);
        }

        // Store Metrics
        GlyphInfo glyph{};
        glyph.u0 = (float)x / width;
        glyph.v0 = (float)y / height;
        glyph.u1 = (float)(x + sdfW) / width;
        glyph.v1 = (float)(y + sdfH) / height;
        glyph.width = (float)gw;
        glyph.height = (float)gh;
        // Adjust bearing to include SDF padding offset?
        // xoff/yoff from stbtt_GetGlyphSDF typically includes the shift. 
        // Logic: cursor at (0,0). Bitmap should be drawn at (xoff, yoff).
        glyph.bearingX = (float)xoff;
        glyph.bearingY = (float)yoff; 
        glyph.advance = advance * scale;

        m_glyphs[static_cast<char>(c)] = glyph;

        x += outW + 1;
        if (y + outH > bottomY) bottomY = y + outH;
    }

    // 5. Upload to Vulkan
    VkDeviceSize imageSize = bitmap.size();
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingBufferMemory;
    m_context.createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingBufferMemory);

    void* data;
    vkMapMemory(m_context.getDevice(), stagingBufferMemory, 0, imageSize, 0, &data);
    memcpy(data, bitmap.data(), static_cast<size_t>(imageSize));
    vkUnmapMemory(m_context.getDevice(), stagingBufferMemory);

    m_context.createImage(width, height, VK_FORMAT_R8_UNORM, VK_IMAGE_TILING_OPTIMAL, 
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 
        m_image, m_memory);

    // Copy commands
    VkCommandBuffer commandBuffer = m_context.beginSingleTimeCommands();

    // Transition Undefined -> Transfer Dst
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};

    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, m_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition Transfer Dst -> Shader Read
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    m_context.endSingleTimeCommands(commandBuffer);

    vkDestroyBuffer(m_context.getDevice(), stagingBuffer, nullptr);
    vkFreeMemory(m_context.getDevice(), stagingBufferMemory, nullptr);

    // Create ImageView
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(m_context.getDevice(), &viewInfo, nullptr, &m_imageView) != VK_SUCCESS) {
        throw std::runtime_error("failed to create SDF image view!");
    }

    // Create Sampler (Linear for SDF!)
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR; // Linear gives smooth edges for SDF
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;

    if (vkCreateSampler(m_context.getDevice(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS) {
        throw std::runtime_error("failed to create SDF sampler!");
    }

    // REGISTER IN BINDLESS
    m_textureID = m_bindlessSystem.registerTexture(m_imageView, m_sampler);
    std::cout << "Font SDF Atlas generated and registered. ID: " << m_textureID << std::endl;
}

} // namespace ui
