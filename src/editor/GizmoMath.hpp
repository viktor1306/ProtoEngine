#pragma once

#include "scene/Scene.hpp"

#include <glm/glm.hpp>
#include <optional>

namespace proto {

// The editor deliberately keeps the manipulator's model independent from ImGui.
// These enums and helpers are also used by the in-memory native smoke scenario.
enum class GizmoOperation { Translate, Rotate, Scale };
enum class GizmoAxis { X, Y, Z, All };

struct GizmoBasis {
    glm::vec3 pivotWorld{};
    glm::mat4 parentWorld{1};
    glm::mat3 parentLinear{1};
    glm::mat3 localRotation{1};
    // This is the local object frame in world space. It intentionally excludes
    // the child's scale: a non-uniform scale must not turn an axis handle into
    // a second, accidental scale operation.
    glm::mat3 worldBasis{1};
};

GizmoBasis makeGizmoBasis(const Transform& local, const glm::mat4& parentWorld, glm::vec3 pivotWorld);
bool finiteMatrix(const glm::mat4& value);
bool invertibleMatrix(const glm::mat4& value, float epsilon = 1.0e-8f);
int axisIndex(GizmoAxis axis);
glm::vec3 unitAxis(GizmoAxis axis);
glm::vec3 basisAxis(const GizmoBasis& basis, GizmoAxis axis);

// Return the signed parameter of the closest point on pivot + axis * t to the
// supplied ray. Both vectors are normalized internally. A nearly parallel ray
// has no stable answer and returns nullopt.
std::optional<float> closestAxisParameter(const Ray& ray, glm::vec3 pivot, glm::vec3 axis, float epsilon = 1.0e-6f);

// Intersect a ray with a plane. The result is rejected when the plane is
// parallel to the ray or lies behind the ray origin.
std::optional<glm::vec3> intersectPlane(const Ray& ray, glm::vec3 point, glm::vec3 normal, float epsilon = 1.0e-6f);

// The ring is an ellipse after a non-uniform/sheared parent transform. The
// returned angle is measured in the selected object's local pre-rotation frame,
// so local-axis rotation remains representable as a quaternion multiplication.
std::optional<float> ringAngle(const Ray& ray, const GizmoBasis& basis, GizmoAxis axis, float epsilon = 1.0e-6f);

std::optional<Transform> applyWorldTranslation(const Transform& start, const glm::mat4& parentWorld,
                                               glm::vec3 desiredWorldPivot);
std::optional<Transform> applyLocalRotation(const Transform& start, GizmoAxis axis, float radians);

// factor is multiplicative and positive. The result preserves each component's
// original sign and clamps the magnitude away from a new singular transform.
std::optional<Transform> applyLocalScale(const Transform& start, GizmoAxis axis, float factor,
                                         float minimumMagnitude = 1.0e-4f, float maximumFactor = 1.0e4f);

float wrapAngle(float radians);

} // namespace proto
