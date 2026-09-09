#include "renderer/LightingMath.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace proto {
Bounds transformBounds(const Bounds& local, const glm::mat4& world) {
    const auto center = glm::vec3(world * glm::vec4((local.min + local.max) * .5f, 1));
    const auto half = (local.max - local.min) * .5f;
    const auto extent = glm::abs(glm::vec3(world[0])) * half.x + glm::abs(glm::vec3(world[1])) * half.y +
                        glm::abs(glm::vec3(world[2])) * half.z;
    return {center - extent, center + extent};
}
ShadowFrustum makeShadowFrustum(const glm::mat4& vp) {
    const auto r = glm::transpose(vp);
    const glm::vec4 planes[]{r[3] + r[0], r[3] - r[0], r[3] + r[1], r[3] - r[1], r[2], r[3] - r[2]};
    ShadowFrustum result;
    for (size_t i = 0; i < result.planes.size(); ++i)
        result.planes[i] = {planes[i], glm::abs(glm::vec3(planes[i]))};
    return result;
}
bool shadowFrustumContains(const Bounds& b, const ShadowFrustum& frustum) {
    const auto center = (b.min + b.max) * .5f, half = (b.max - b.min) * .5f;
    constexpr float relativeEpsilon = 8.0f * std::numeric_limits<float>::epsilon();
    for (const auto& plane : frustum.planes) {
        const auto& p = plane.equation;
        const float centerDistance = glm::dot(glm::vec3(p), center) + p.w;
        const float extent = glm::dot(plane.absoluteNormal, half);
        const float tolerance = relativeEpsilon * std::max({1.0f, std::abs(centerDistance), std::abs(extent)});
        if (centerDistance + extent < -tolerance)
            return false;
    }
    return true;
}
bool shadowFrustumContains(const Bounds& b, const glm::mat4& vp) {
    return shadowFrustumContains(b, makeShadowFrustum(vp));
}
bool sphereTouches(const Bounds& b, glm::vec3 p, float radius) {
    const auto delta = p - glm::clamp(p, b.min, b.max);
    const float distanceSquared = glm::dot(delta, delta), radiusSquared = radius * radius;
    constexpr float relativeEpsilon = 8.0f * std::numeric_limits<float>::epsilon();
    const float tolerance = relativeEpsilon * std::max({1.0f, std::abs(distanceSquared), std::abs(radiusSquared)});
    return distanceSquared <= radiusSquared + tolerance;
}
std::array<glm::mat4, 6> pointShadowMatrices(glm::vec3 p, float radius) {
    const glm::vec3 directions[]{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const glm::vec3 up[]{{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    // Point-shadow depth is written as radial distance in shadow.frag. Keep
    // the projection near clip at a fixed sub-5mm world distance so a large
    // radius does not discard close casters before the radial depth write.
    const auto projection = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, .005f, radius);
    std::array<glm::mat4, 6> result;
    // Positive viewport: these bases match Vulkan's cube sc/tc table directly.
    for (size_t i = 0; i < 6; ++i)
        result[i] = projection * glm::lookAtRH(p, p + directions[i], up[i]);
    return result;
}
CascadeSet directionalCascades(const RenderView& view, glm::vec3 direction, float aspect,
                               const std::vector<Bounds>& casters) {
    CascadeSet result;
    const auto count = view.lighting.shadowCascades;
    const float near = view.cameraNear,
                far = std::min(view.cameraFar, std::max(near + .01f, view.lighting.shadowDistance));
    const auto cameraWorld = glm::inverse(view.cameraView);
    const glm::vec3 eye(cameraWorld[3]), right(cameraWorld[0]), up(cameraWorld[1]), forward(-cameraWorld[2]);
    const float tangent = std::tan(glm::radians(view.verticalFovDegrees) * .5f);
    const auto helper = std::abs(direction.y) > .95f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    const auto rotation = glm::lookAtRH(glm::vec3(0), glm::normalize(direction), helper);
    // Every cascade uses this same light-space rotation. Transform each
    // caster once, retaining off-camera casters for all cascade depth ranges.
    std::vector<Bounds> lightBounds;
    lightBounds.reserve(casters.size());
    for (const auto& bounds : casters)
        lightBounds.push_back(transformBounds(bounds, rotation));
    float previous = near;
    for (uint32_t cascade = 0; cascade < count; ++cascade) {
        const float fraction = float(cascade + 1) / float(count);
        const float split = .6f * near * std::pow(far / near, fraction) + .4f * (near + (far - near) * fraction);
        result.splits[static_cast<int>(cascade)] = split;
        // Small overlap supports a smooth blend over the end of the previous cascade.
        const float begin = cascade ? std::max(near, previous - (previous - near) * .12f) : near;
        std::array<glm::vec3, 8> corners;
        size_t at{};
        for (float depth : {begin, split})
            for (float y : {-1.0f, 1.0f})
                for (float x : {-1.0f, 1.0f})
                    corners[at++] =
                        eye + forward * depth + right * (x * depth * tangent * aspect) + up * (y * depth * tangent);
        glm::vec3 center(0);
        for (auto p : corners)
            center += p;
        center /= 8.0f;
        float radius{};
        for (auto p : corners)
            radius = std::max(radius, glm::length(p - center));
        // Quantize the unpadded radius first so small camera movements keep
        // the same footprint. Reserve one full final texel: half a texel for
        // center snapping and half a texel for the 2x2 PCF footprint. Solving
        // R = base + 2R/resolution keeps the margin tied to a bounded,
        // camera-stable radius rather than varying it after snapping.
        const float baseRadius = std::ceil(radius * 16) / 16;
        const float resolution = float(view.lighting.shadowResolution);
        const float texel = 2 * baseRadius / std::max(1.0f, resolution - 2.0f);
        radius = baseRadius + texel;
        const float snapTexel = 2 * radius / resolution;
        auto lightCenter = glm::vec3(rotation * glm::vec4(center, 1));
        lightCenter.x = std::floor(lightCenter.x / snapTexel + .5f) * snapTexel;
        lightCenter.y = std::floor(lightCenter.y / snapTexel + .5f) * snapTexel;
        float minZ = lightCenter.z - radius, maxZ = lightCenter.z + radius;
        for (const auto& bounds : lightBounds) {
            if (bounds.max.x < lightCenter.x - radius || bounds.min.x > lightCenter.x + radius ||
                bounds.max.y < lightCenter.y - radius || bounds.min.y > lightCenter.y + radius)
                continue;
            minZ = std::min(minZ, bounds.min.z);
            maxZ = std::max(maxZ, bounds.max.z);
        }
        minZ = std::floor(minZ - 1);
        maxZ = std::ceil(maxZ + 1);
        result.matrices[cascade] = glm::orthoRH_ZO(lightCenter.x - radius, lightCenter.x + radius,
                                                   lightCenter.y - radius, lightCenter.y + radius, -maxZ, -minZ) *
                                   rotation;
        previous = split;
    }
    for (uint32_t i = count; i < 4; ++i)
        result.splits[static_cast<int>(i)] = far;
    return result;
}
} // namespace proto
