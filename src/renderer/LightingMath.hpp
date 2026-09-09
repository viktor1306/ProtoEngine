#pragma once
#include "renderer/RenderView.hpp"
#include <array>

namespace proto {
struct CascadeSet {
    std::array<glm::mat4, 4> matrices{glm::mat4(1), glm::mat4(1), glm::mat4(1), glm::mat4(1)};
    glm::vec4 splits{};
};
Bounds transformBounds(const Bounds& local, const glm::mat4& world);
struct ShadowFrustum {
    struct Plane {
        glm::vec4 equation;
        glm::vec3 absoluteNormal;
    };
    std::array<Plane, 6> planes;
};
ShadowFrustum makeShadowFrustum(const glm::mat4& viewProjection);
bool shadowFrustumContains(const Bounds& bounds, const ShadowFrustum& frustum);
bool shadowFrustumContains(const Bounds& bounds, const glm::mat4& viewProjection);
bool sphereTouches(const Bounds& bounds, glm::vec3 position, float radius);
std::array<glm::mat4, 6> pointShadowMatrices(glm::vec3 position, float radius);
CascadeSet directionalCascades(const RenderView& view, glm::vec3 direction, float aspect,
                               const std::vector<Bounds>& casters);
} // namespace proto
