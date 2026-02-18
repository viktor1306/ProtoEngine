#include "Frustum.hpp"
#include <cmath>

namespace scene {

// ---------------------------------------------------------------------------
// extractPlanes — Gribb/Hartmann method
//
// Our Mat4 is column-major: data[col][row]
// Row i of the matrix = { data[0][i], data[1][i], data[2][i], data[3][i] }
//
// For a column-major ViewProjection matrix M, the clip-space position is:
//   p_clip = M * p_world
//
// The 6 frustum planes in world space are:
//   Left:   row3 + row0  (clip.w + clip.x >= 0)
//   Right:  row3 - row0  (clip.w - clip.x >= 0)
//   Bottom: row3 + row1  (clip.w + clip.y >= 0)
//   Top:    row3 - row1  (clip.w - clip.y >= 0)
//   Near:   row2         (clip.z >= 0)  — Vulkan depth [0,1]
//   Far:    row3 - row2  (clip.w - clip.z >= 0)
//
// Each plane is normalized so that distanceTo() gives true signed distance.
// ---------------------------------------------------------------------------
void Frustum::extractPlanes(const core::math::Mat4& m) {
    // Helper: extract row i from column-major matrix
    // row[i] = { m.data[0][i], m.data[1][i], m.data[2][i], m.data[3][i] }
    auto row = [&](int i) -> core::math::Vec4 {
        return { m.data[0][i], m.data[1][i], m.data[2][i], m.data[3][i] };
    };

    auto r0 = row(0);
    auto r1 = row(1);
    auto r2 = row(2);
    auto r3 = row(3);

    // Combine rows into plane equations (nx, ny, nz, d)
    // plane.normal = (nx, ny, nz), plane.d = w component
    auto makePlane = [](core::math::Vec4 a, core::math::Vec4 b, float signB) -> Plane {
        Plane p;
        p.normal.x = a.x + signB * b.x;
        p.normal.y = a.y + signB * b.y;
        p.normal.z = a.z + signB * b.z;
        p.d        = a.w + signB * b.w;
        // Normalize
        float len = std::sqrt(p.normal.x*p.normal.x +
                              p.normal.y*p.normal.y +
                              p.normal.z*p.normal.z);
        if (len > 1e-6f) {
            float inv = 1.0f / len;
            p.normal.x *= inv;
            p.normal.y *= inv;
            p.normal.z *= inv;
            p.d        *= inv;
        }
        return p;
    };

    m_planes[0] = makePlane(r3,  r0, +1.0f); // Left:   row3 + row0
    m_planes[1] = makePlane(r3,  r0, -1.0f); // Right:  row3 - row0
    m_planes[2] = makePlane(r3,  r1, +1.0f); // Bottom: row3 + row1
    m_planes[3] = makePlane(r3,  r1, -1.0f); // Top:    row3 - row1
    m_planes[4] = makePlane(r2,  r3, +0.0f); // Near:   row2 (Vulkan depth [0,1])
    m_planes[5] = makePlane(r3,  r2, -1.0f); // Far:    row3 - row2
}

// ---------------------------------------------------------------------------
// isAABBOutside — "positive vertex" test
//
// For each plane, find the vertex of the AABB that is most in the direction
// of the plane normal (the "positive vertex" p+). If p+ is behind the plane,
// the entire AABB is outside → cull it.
//
// p+ for axis i: if normal[i] >= 0 → use max[i], else use min[i]
// ---------------------------------------------------------------------------
bool Frustum::isAABBOutside(const AABB& box) const {
    // Test all 6 planes.
    // Vulkan Y-flip: perspective has data[1][1] < 0, so Bottom/Top planes
    // extracted from clip.y are swapped relative to world Y.
    // The positive-vertex test still works correctly because we pick the
    // vertex most in the direction of the (possibly inverted) normal —
    // the math is self-consistent as long as we use the same VP matrix
    // for both plane extraction and the actual vertex transform.
    for (int i = 0; i < 6; ++i) {
        const Plane& pl = m_planes[i];

        // Positive vertex (most in direction of normal)
        core::math::Vec3 pv;
        pv.x = (pl.normal.x >= 0.0f) ? box.max.x : box.min.x;
        pv.y = (pl.normal.y >= 0.0f) ? box.max.y : box.min.y;
        pv.z = (pl.normal.z >= 0.0f) ? box.max.z : box.min.z;

        // If positive vertex is behind this plane → AABB fully outside
        if (pl.distanceTo(pv) < 0.0f) return true;
    }
    return false;
}

} // namespace scene
