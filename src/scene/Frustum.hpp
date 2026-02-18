#pragma once

#include "core/Math.hpp"

namespace scene {

// ---------------------------------------------------------------------------
// Plane — half-space defined by normal + distance
// Equation: dot(normal, P) + d = 0
// ---------------------------------------------------------------------------
struct Plane {
    core::math::Vec3 normal{0,0,0};
    float d = 0.0f;

    // Signed distance from point to plane (positive = in front)
    float distanceTo(const core::math::Vec3& p) const {
        return core::math::Vec3::dot(normal, p) + d;
    }
};

// ---------------------------------------------------------------------------
// AABB — Axis-Aligned Bounding Box
// ---------------------------------------------------------------------------
struct AABB {
    core::math::Vec3 min{0,0,0};
    core::math::Vec3 max{0,0,0};

    core::math::Vec3 center()  const { return {(min.x+max.x)*0.5f, (min.y+max.y)*0.5f, (min.z+max.z)*0.5f}; }
    core::math::Vec3 extents() const { return {(max.x-min.x)*0.5f, (max.y-min.y)*0.5f, (max.z-min.z)*0.5f}; }
};

// ---------------------------------------------------------------------------
// Frustum — 6-plane view frustum
//
// Planes are extracted from the combined ViewProjection matrix using the
// Gribb/Hartmann method (adapted for column-major Mat4 where data[col][row]).
//
// Plane order: Left, Right, Bottom, Top, Near, Far
// ---------------------------------------------------------------------------
class Frustum {
public:
    Frustum() = default;

    // Extract 6 frustum planes from a combined ViewProjection matrix.
    // Mat4 is column-major: data[col][row], so row i = {data[0][i], data[1][i], data[2][i], data[3][i]}
    void extractPlanes(const core::math::Mat4& vp);

    // Returns true if the AABB is fully outside the frustum (should be culled).
    // Uses the "positive vertex" test for each plane — fast conservative check.
    bool isAABBOutside(const AABB& box) const;

    // Returns true if the AABB is visible (intersects or inside frustum).
    bool isVisible(const AABB& box) const { return !isAABBOutside(box); }

private:
    Plane m_planes[6]; // Left, Right, Bottom, Top, Near, Far
};

} // namespace scene
