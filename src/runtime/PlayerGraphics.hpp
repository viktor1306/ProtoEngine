#pragma once

#include "renderer/RenderView.hpp"
#include <volk.h>
#include <cstdint>
#include <string>
#include <string_view>

namespace proto {

struct PlayerSunShadowSettings {
    bool enabled{true};
    uint32_t cascades{3};
    uint32_t resolution{2048};
    float distance{80};
    std::string filter{"pcf3x3"};
};

struct PlayerPointShadowSettings {
    bool enabled{true};
    uint32_t resolution{512};
    uint32_t filterTaps{9};
    uint32_t poolBudgetMiB{96};
    std::string overflow{"multiPass"};
};

struct PlayerGraphics {
    std::string profile{"Balanced"};
    float renderScale{1};
    bool vSync{true};
    float viewDistance{300};
    PlayerSunShadowSettings sunShadows;
    PlayerPointShadowSettings pointShadows;
    uint32_t textureTopMipDrop{};
};

void validatePlayerGraphics(const PlayerGraphics& settings);

// Strict proto.graphics version-1 decoding/encoding. Missing optional
// textureTopMipDrop is treated as zero for M5-era packages.
PlayerGraphics decodePlayerGraphics(std::string_view bytes);
std::string encodePlayerGraphics(const PlayerGraphics& settings);
PlayerGraphics playerGraphicsProfile(std::string_view profile);

// Applies package/runtime settings to a renderer view without mutating the
// authored Scene. A view with explicitSettings=false keeps the M5 shared
// lighting path and legacy scene defaults.
void applyPlayerGraphics(RenderView& view, VkExtent2D extent, const PlayerGraphics& settings);

} // namespace proto
