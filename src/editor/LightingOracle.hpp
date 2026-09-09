#pragma once

#include <glm/glm.hpp>

namespace proto {
class Scene;
struct RenderView;

/**
 * Evaluate the native ground-plane lighting path with a small, independent
 * CPU oracle. The result is display-space RGB in the inclusive 0..255 range.
 *
 * Shadow visibility is intentionally a CPU ray result. It does not reproduce
 * renderer shadow-map texel filtering, cascade selection/range edges, or
 * rasterizer bias exactly; GPU fixtures should therefore sample the interiors
 * of lit/shadowed regions. Interior probes receive the actual CPU visibility
 * result from the scene's visible shadow casters.
 */
glm::vec3 expectedGroundRgb(const Scene& scene, const RenderView& view, glm::vec3 groundPosition, glm::vec3 albedo,
                            bool receiveShadows = true);
bool stableGroundVisibility(const Scene& scene, const RenderView& view, glm::vec3 groundPosition, float footprint);
} // namespace proto
