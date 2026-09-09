#include "editor/GizmoMath.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace proto {
namespace {
constexpr float pi = 3.14159265358979323846f;

bool finiteVector(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool validAxis(glm::vec3 value) {
    const float length = glm::length(value);
    return finiteVector(value) && std::isfinite(length) && length > 1.0e-8f;
}

std::pair<int, int> ringIndices(GizmoAxis axis) {
    const int i = axisIndex(axis);
    return {(i + 1) % 3, (i + 2) % 3};
}
} // namespace

GizmoBasis makeGizmoBasis(const Transform& local, const glm::mat4& parentWorld, glm::vec3 pivotWorld) {
    GizmoBasis result;
    result.pivotWorld = pivotWorld;
    result.parentWorld = parentWorld;
    result.parentLinear = glm::mat3(parentWorld);
    result.localRotation = glm::mat3_cast(glm::normalize(local.rotation));
    result.worldBasis = result.parentLinear * result.localRotation;
    return result;
}

bool finiteMatrix(const glm::mat4& value) {
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            if (!std::isfinite(value[column][row]))
                return false;
    return true;
}

bool invertibleMatrix(const glm::mat4& value, float epsilon) {
    if (!finiteMatrix(value))
        return false;
    const float determinant = glm::determinant(glm::mat3(value));
    return std::isfinite(determinant) && std::abs(determinant) >= epsilon;
}

int axisIndex(GizmoAxis axis) {
    switch (axis) {
    case GizmoAxis::X:
        return 0;
    case GizmoAxis::Y:
        return 1;
    case GizmoAxis::Z:
        return 2;
    case GizmoAxis::All:
        return -1;
    }
    return -1;
}

glm::vec3 unitAxis(GizmoAxis axis) {
    switch (axis) {
    case GizmoAxis::X:
        return {1, 0, 0};
    case GizmoAxis::Y:
        return {0, 1, 0};
    case GizmoAxis::Z:
        return {0, 0, 1};
    case GizmoAxis::All:
        return {0, 0, 0};
    }
    return {0, 0, 0};
}

glm::vec3 basisAxis(const GizmoBasis& basis, GizmoAxis axis) {
    const int index = axisIndex(axis);
    if (index < 0)
        return {};
    const glm::vec3 value = basis.worldBasis[index];
    const float length = glm::length(value);
    return std::isfinite(length) && length > 1.0e-8f ? value / length : glm::vec3{};
}

std::optional<float> closestAxisParameter(const Ray& input, glm::vec3 pivot, glm::vec3 axis, float epsilon) {
    if (!finiteVector(input.origin) || !finiteVector(input.direction) || !finiteVector(pivot) || !validAxis(axis))
        return {};
    const float directionLength = glm::length(input.direction);
    const float axisLength = glm::length(axis);
    if (!std::isfinite(directionLength) || !std::isfinite(axisLength) || directionLength <= epsilon ||
        axisLength <= epsilon)
        return {};
    const glm::vec3 direction = input.direction / directionLength;
    axis /= axisLength;
    const glm::vec3 offset = input.origin - pivot;
    const float cosine = glm::dot(direction, axis);
    const float denominator = 1.0f - cosine * cosine;
    if (!std::isfinite(denominator) || denominator <= epsilon)
        return {};
    const float result = (glm::dot(axis, offset) - cosine * glm::dot(direction, offset)) / denominator;
    return std::isfinite(result) ? std::optional<float>(result) : std::nullopt;
}

std::optional<glm::vec3> intersectPlane(const Ray& input, glm::vec3 point, glm::vec3 normal, float epsilon) {
    if (!finiteVector(input.origin) || !finiteVector(input.direction) || !finiteVector(point) || !validAxis(normal))
        return {};
    const float directionLength = glm::length(input.direction);
    const float normalLength = glm::length(normal);
    if (!std::isfinite(directionLength) || !std::isfinite(normalLength) || directionLength <= epsilon ||
        normalLength <= epsilon)
        return {};
    const glm::vec3 direction = input.direction / directionLength;
    normal /= normalLength;
    const float denominator = glm::dot(direction, normal);
    if (!std::isfinite(denominator) || std::abs(denominator) <= epsilon)
        return {};
    const float distance = glm::dot(point - input.origin, normal) / denominator;
    if (!std::isfinite(distance) || distance < 0.0f)
        return {};
    const glm::vec3 result = input.origin + direction * distance;
    return finiteVector(result) ? std::optional<glm::vec3>(result) : std::nullopt;
}

std::optional<float> ringAngle(const Ray& ray, const GizmoBasis& basis, GizmoAxis axis, float epsilon) {
    const int selected = axisIndex(axis);
    if (selected < 0 || !invertibleMatrix(basis.parentWorld, epsilon))
        return {};
    const auto [uIndex, vIndex] = ringIndices(axis);
    const glm::vec3 u = basis.worldBasis[uIndex];
    const glm::vec3 v = basis.worldBasis[vIndex];
    const glm::vec3 normal = glm::cross(u, v);
    if (!validAxis(u) || !validAxis(v) || !validAxis(normal))
        return {};
    const auto hit = intersectPlane(ray, basis.pivotWorld, normal, epsilon);
    if (!hit)
        return {};
    const glm::vec3 local = glm::inverse(basis.worldBasis) * (*hit - basis.pivotWorld);
    if (!finiteVector(local) || (std::abs(local[uIndex]) <= epsilon && std::abs(local[vIndex]) <= epsilon))
        return {};
    const float angle = std::atan2(local[vIndex], local[uIndex]);
    return std::isfinite(angle) ? std::optional<float>(angle) : std::nullopt;
}

std::optional<Transform> applyWorldTranslation(const Transform& start, const glm::mat4& parentWorld,
                                               glm::vec3 desiredWorldPivot) {
    if (!finiteVector(desiredWorldPivot) || !invertibleMatrix(parentWorld))
        return {};
    const glm::vec4 local = glm::inverse(parentWorld) * glm::vec4(desiredWorldPivot, 1.0f);
    if (!std::isfinite(local.w) || std::abs(local.w) <= 1.0e-8f)
        return {};
    const glm::vec3 position = glm::vec3(local) / local.w;
    if (!finiteVector(position))
        return {};
    Transform result = start;
    result.position = position;
    return result;
}

std::optional<Transform> applyLocalRotation(const Transform& start, GizmoAxis axis, float radians) {
    const int index = axisIndex(axis);
    const float rotationLength = glm::length(start.rotation);
    if (index < 0 || !std::isfinite(radians) || !std::isfinite(rotationLength) || rotationLength <= 1.0e-8f)
        return {};
    Transform result = start;
    result.rotation = glm::normalize(start.rotation * glm::angleAxis(radians, unitAxis(axis)));
    return std::isfinite(glm::length(result.rotation)) ? std::optional<Transform>(result) : std::nullopt;
}

std::optional<Transform> applyLocalScale(const Transform& start, GizmoAxis axis, float factor, float minimumMagnitude,
                                         float maximumFactor) {
    if (!std::isfinite(factor) || !std::isfinite(minimumMagnitude) || !std::isfinite(maximumFactor) ||
        minimumMagnitude <= 0.0f || maximumFactor < minimumMagnitude || factor <= 0.0f)
        return {};
    const int selected = axisIndex(axis);
    if (selected < 0 && axis != GizmoAxis::All)
        return {};
    const float safeFactor = std::clamp(factor, minimumMagnitude, maximumFactor);
    Transform result = start;
    const auto apply = [&](int index) {
        const float source = start.scale[index];
        if (!std::isfinite(source))
            return false;
        if (std::abs(source) <= std::numeric_limits<float>::epsilon() && std::abs(safeFactor - 1.0f) <= 1.0e-6f) {
            result.scale[index] = source;
            return true;
        }
        const float sign = source < 0.0f ? -1.0f : 1.0f;
        const float magnitude = std::max(std::abs(source) * safeFactor, minimumMagnitude);
        if (!std::isfinite(magnitude))
            return false;
        result.scale[index] = sign * magnitude;
        return true;
    };
    if (axis == GizmoAxis::All) {
        if (!apply(0) || !apply(1) || !apply(2))
            return {};
    } else if (!apply(selected))
        return {};
    return result;
}

float wrapAngle(float radians) {
    if (!std::isfinite(radians))
        return 0.0f;
    while (radians > pi)
        radians -= 2.0f * pi;
    while (radians < -pi)
        radians += 2.0f * pi;
    return radians;
}

} // namespace proto
