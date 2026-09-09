#include "editor/EditorCamera.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace proto {
glm::vec3 EditorCamera::eye() const { return pivot + distance * glm::vec3(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)); }
glm::mat4 EditorCamera::view() const { return glm::lookAtRH(eye(), pivot, glm::vec3(0, 1, 0)); }
void EditorCamera::focus(const Bounds& bounds) { pivot = (bounds.min + bounds.max) * .5f; distance = std::clamp(glm::length(bounds.max - bounds.min) * 1.5f, 1.5f, 10000.0f); }
Ray viewportRay(const glm::mat4& viewProjection, float u, float v) {
    const auto inverse = glm::inverse(viewProjection);
    auto near = inverse * glm::vec4(u * 2 - 1, 1 - v * 2, 0, 1), far = inverse * glm::vec4(u * 2 - 1, 1 - v * 2, 1, 1);
    near /= near.w; far /= far.w;
    return {glm::vec3(near), glm::normalize(glm::vec3(far - near))};
}
bool inFrustum(const Bounds& bounds, const glm::mat4& vp) {
    const auto rows = glm::transpose(vp);
    const glm::vec4 planes[]{rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1], rows[3] - rows[1], rows[2], rows[3] - rows[2]};
    const glm::vec3 center = (bounds.min + bounds.max) * .5f, half = (bounds.max - bounds.min) * .5f;
    for (const auto& plane : planes) if (glm::dot(glm::vec3(plane), center) + plane.w + glm::dot(glm::abs(glm::vec3(plane)), half) < 0) return false;
    return true;
}
}
