#pragma once
#include "scene/Scene.hpp"

namespace proto {
class EditorCamera {
public:
    glm::vec3 pivot{0, .5f, 0};
    float yaw{.65f}, pitch{.45f}, distance{8};
    glm::vec3 eye() const;
    glm::mat4 view() const;
    void focus(const Bounds& bounds);
};
Ray viewportRay(const glm::mat4& viewProjection, float u, float v);
bool inFrustum(const Bounds& bounds, const glm::mat4& viewProjection);
}
