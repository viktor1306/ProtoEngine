#pragma once
#include <glm/glm.hpp>
#include <cstdint>

namespace proto {
struct DirectionalLight {
    glm::vec3 color{1.0f, .95f, .85f};
    float intensity{3};
    bool shadows{true};
    bool operator==(const DirectionalLight&) const = default;
};
struct PointLight {
    glm::vec3 color{1};
    float intensity{30}, radius{8};
    bool shadows{true};
    bool operator==(const PointLight&) const = default;
};
struct LightingSettings {
    uint32_t shadowResolution{512}, shadowCascades{3}, shadowPoolMiB{64};
    float shadowDistance{40}, depthBias{.001f}, normalBias{.015f}, ambient{.025f};
    bool shadows{true}, filteredShadows{true}, referenceLighting{false}, forceShadowRefresh{false};
    bool operator==(const LightingSettings&) const = default;
};
void validateLighting(const LightingSettings& settings);
} // namespace proto
