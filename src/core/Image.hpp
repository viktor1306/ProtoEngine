#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace proto {
void savePng(const std::filesystem::path& path, uint32_t width, uint32_t height, std::span<const uint8_t> rgba);
struct PixelChecks {
    bool yUp{};
    bool frontFace{};
    bool backFaceCulled{};
    bool depthOcclusion{};
    bool clearDepth{};
    bool passed() const { return yUp && frontFace && backFaceCulled && depthOcclusion && clearDepth; }
};
PixelChecks checkTriangle(uint32_t width, uint32_t height, std::span<const uint8_t> rgba, std::span<const float> depth);
}
