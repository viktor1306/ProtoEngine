#include "editor/LightingOracle.hpp"

#include "assets/AssetTypes.hpp"
#include "renderer/RenderView.hpp"
#include "scene/Scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace proto {
namespace {

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double rayEpsilon = 1.0e-7;
constexpr double terminalEpsilon = 1.0e-5;
constexpr double cullEpsilon = 1.0e-12;

using DVec3 = glm::dvec3;
using DMat4 = glm::dmat4;

enum class CasterKind { Imported, Cube, Plane };

struct Caster {
    CasterKind kind{};
    DMat4 world{1.0};
    DMat4 inverseWorld{1.0};
    DVec3 boundsMin{0.0};
    DVec3 boundsMax{0.0};
    const MeshAsset* mesh{};
    std::vector<AssetId> materials;
};

bool finite(double value) {
    return std::isfinite(value);
}

bool finite(const DVec3& value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

bool finite(const DMat4& value) {
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            if (!finite(value[column][row]))
                return false;
    return true;
}

DVec3 toDouble(glm::vec3 value) {
    return {static_cast<double>(value.x), static_cast<double>(value.y), static_cast<double>(value.z)};
}

glm::vec3 toFloat(DVec3 value) {
    return {static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z)};
}

DMat4 toDouble(glm::mat4 value) {
    DMat4 result(1.0);
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            result[column][row] = static_cast<double>(value[column][row]);
    return result;
}

void includePoint(DVec3 point, DVec3& minimum, DVec3& maximum) {
    minimum = glm::min(minimum, point);
    maximum = glm::max(maximum, point);
}

bool localBounds(const Caster& caster, DVec3& minimum, DVec3& maximum) {
    if (caster.kind == CasterKind::Plane) {
        minimum = {-0.5, 0.0, -0.5};
        maximum = {0.5, 0.0, 0.5};
        return true;
    }
    if (caster.kind == CasterKind::Cube) {
        minimum = {-0.5, -0.5, -0.5};
        maximum = {0.5, 0.5, 0.5};
        return true;
    }
    if (!caster.mesh || !finite(toDouble(caster.mesh->bounds.min)) || !finite(toDouble(caster.mesh->bounds.max)))
        return false;
    minimum = toDouble(caster.mesh->bounds.min);
    maximum = toDouble(caster.mesh->bounds.max);
    return finite(minimum) && finite(maximum) && minimum.x <= maximum.x && minimum.y <= maximum.y &&
           minimum.z <= maximum.z;
}

bool worldBounds(const Caster& caster, DVec3& minimum, DVec3& maximum) {
    DVec3 localMinimum{}, localMaximum{};
    if (!localBounds(caster, localMinimum, localMaximum))
        return false;
    minimum = DVec3(std::numeric_limits<double>::max());
    maximum = DVec3(std::numeric_limits<double>::lowest());
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) {
                const DVec3 corner{x ? localMaximum.x : localMinimum.x, y ? localMaximum.y : localMinimum.y,
                                   z ? localMaximum.z : localMinimum.z};
                const auto transformed = caster.world * glm::dvec4(corner, 1.0);
                if (!finite(transformed.x) || !finite(transformed.y) || !finite(transformed.z) ||
                    !finite(transformed.w) || std::abs(transformed.w) < cullEpsilon)
                    return false;
                includePoint(DVec3(transformed) / transformed.w, minimum, maximum);
            }
    return finite(minimum) && finite(maximum);
}

std::vector<Caster> collectCasters(const Scene& scene) {
    std::vector<Caster> result;
    result.reserve(scene.meshes().size());
    for (const auto handle : scene.meshes()) {
        const auto* renderer = scene.mesh(handle);
        if (!renderer || !renderer->castShadows)
            continue;
        const auto& entity = scene.entity(handle);
        const auto& transform = scene.transform(handle);
        // TransformState::visible includes enabled ancestors. Keep the direct
        // check too so this remains explicit if visibility bookkeeping changes.
        if (!entity.enabled || !transform.visible || transform.degenerate)
            continue;

        Caster caster;
        if (renderer->mesh == builtin::plane)
            caster.kind = CasterKind::Plane;
        else if (renderer->mesh == builtin::cube)
            caster.kind = CasterKind::Cube;
        else {
            if (!scene.assets)
                continue;
            const auto found = scene.assets->meshes.find(renderer->mesh);
            if (found == scene.assets->meshes.end() || !found->second)
                continue;
            caster.kind = CasterKind::Imported;
            caster.mesh = found->second.get();
            caster.materials = renderer->materials;
        }

        caster.world = toDouble(transform.world);
        if (!finite(caster.world))
            continue;
        const auto linear = glm::dmat3(caster.world);
        const double determinant = glm::determinant(linear);
        if (!finite(determinant) || std::abs(determinant) <= cullEpsilon)
            continue;
        caster.inverseWorld = glm::inverse(caster.world);
        if (!finite(caster.inverseWorld))
            continue;

        if (!worldBounds(caster, caster.boundsMin, caster.boundsMax)) {
            // A valid Scene update already has this bound. It is a useful
            // fallback for an imported asset whose authored bounds are absent.
            caster.boundsMin = toDouble(transform.bounds.min);
            caster.boundsMax = toDouble(transform.bounds.max);
            if (!finite(caster.boundsMin) || !finite(caster.boundsMax))
                continue;
        }
        result.push_back(std::move(caster));
    }
    return result;
}

bool rayAabb(DVec3 origin, DVec3 direction, DVec3 minimum, DVec3 maximum, double maximumDistance) {
    double nearDistance = 0.0;
    double farDistance = maximumDistance;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) <= cullEpsilon) {
            if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis])
                return false;
            continue;
        }
        double a = (minimum[axis] - origin[axis]) / direction[axis];
        double b = (maximum[axis] - origin[axis]) / direction[axis];
        if (a > b)
            std::swap(a, b);
        nearDistance = std::max(nearDistance, a);
        farDistance = std::min(farDistance, b);
        if (nearDistance > farDistance)
            return false;
    }
    return farDistance >= std::max(0.0, nearDistance) && farDistance >= 0.0;
}

std::optional<double> intersectPlaneLocal(DVec3 origin, DVec3 direction, double maximumDistance) {
    // PrimitiveGeometry's plane has a +Y winding and is one-sided. Keeping
    // this local-space test also makes mirrored transforms follow the native
    // renderer's winding-flipped pipeline.
    if (direction.y >= -cullEpsilon)
        return {};
    const double distance = -origin.y / direction.y;
    if (!finite(distance) || distance < 0.0 || distance > maximumDistance)
        return {};
    const DVec3 hit = origin + direction * distance;
    if (hit.x < -0.5 - terminalEpsilon || hit.x > 0.5 + terminalEpsilon || hit.z < -0.5 - terminalEpsilon ||
        hit.z > 0.5 + terminalEpsilon)
        return {};
    return distance;
}

std::optional<double> intersectCubeLocal(DVec3 origin, DVec3 direction, double maximumDistance) {
    const DVec3 minimum{-0.5, -0.5, -0.5};
    const DVec3 maximum{0.5, 0.5, 0.5};
    if (!rayAabb(origin, direction, minimum, maximum, maximumDistance))
        return {};

    struct Face {
        int axis;
        double side;
        DVec3 normal;
    };
    constexpr std::array<Face, 6> faces{{
        {0, -0.5, DVec3{-1.0, 0.0, 0.0}},
        {0, 0.5, DVec3{1.0, 0.0, 0.0}},
        {1, -0.5, DVec3{0.0, -1.0, 0.0}},
        {1, 0.5, DVec3{0.0, 1.0, 0.0}},
        {2, -0.5, DVec3{0.0, 0.0, -1.0}},
        {2, 0.5, DVec3{0.0, 0.0, 1.0}},
    }};
    std::optional<double> result;
    for (const auto& face : faces) {
        const double denominator = glm::dot(face.normal, direction);
        // One-sided native geometry: only a front-facing side can be hit.
        if (denominator >= -cullEpsilon)
            continue;
        const double distance = (face.side - origin[face.axis]) / direction[face.axis];
        if (!finite(distance) || distance < 0.0 || distance > maximumDistance)
            continue;
        const DVec3 hit = origin + direction * distance;
        bool inside = true;
        for (int axis = 0; axis < 3; ++axis)
            if (axis != face.axis &&
                (hit[axis] < minimum[axis] - terminalEpsilon || hit[axis] > maximum[axis] + terminalEpsilon))
                inside = false;
        if (inside && (!result || distance < *result))
            result = distance;
    }
    return result;
}

std::optional<double> intersectCaster(const Caster& caster, const AssetCatalog* assets, DVec3 origin, DVec3 direction,
                                      double maximumDistance) {
    if (maximumDistance < 0.0 || !finite(origin) || !finite(direction) || !finite(maximumDistance))
        return {};
    const auto localOrigin4 = caster.inverseWorld * glm::dvec4(origin, 1.0);
    const auto localDirection4 = caster.inverseWorld * glm::dvec4(direction, 0.0);
    if (!finite(localOrigin4.x) || !finite(localOrigin4.y) || !finite(localOrigin4.z) || !finite(localOrigin4.w) ||
        !finite(localDirection4.x) || !finite(localDirection4.y) || !finite(localDirection4.z) ||
        !finite(localDirection4.w) || std::abs(localOrigin4.w) < cullEpsilon)
        return {};
    const DVec3 localOrigin = DVec3(localOrigin4) / localOrigin4.w;
    const DVec3 localDirectionRaw{localDirection4.x, localDirection4.y, localDirection4.z};
    const double localScale = glm::length(localDirectionRaw);
    if (!finite(localScale) || localScale <= cullEpsilon)
        return {};
    // pickMesh and the analytic primitives report a parameter along their
    // supplied ray direction. Normalize in local space so converting the hit
    // back to the world ray is exactly distance = localHit / localScale;
    // non-uniform scene scales must not silently change the shadow segment.
    const DVec3 localDirection = localDirectionRaw / localScale;
    const double localMaximum = maximumDistance * localScale;
    if (!finite(localMaximum))
        return {};

    std::optional<double> localHit;
    if (caster.kind == CasterKind::Plane)
        localHit = intersectPlaneLocal(localOrigin, localDirection, localMaximum);
    else if (caster.kind == CasterKind::Cube)
        localHit = intersectCubeLocal(localOrigin, localDirection, localMaximum);
    else if (caster.mesh && assets) {
        // pickMesh is deliberately reused here: it applies material-slot
        // selection, MASK alpha testing, and double-sided culling exactly as
        // the asset picking path does.
        const glm::vec3 localOriginFloat = toFloat(localOrigin);
        const glm::vec3 localDirectionFloat = toFloat(localDirection);
        const float localMaximumFloat = static_cast<float>(localMaximum);
        if (std::isfinite(localMaximumFloat)) {
            if (const auto hit = pickMesh(*caster.mesh, *assets, caster.materials, localOriginFloat,
                                          localDirectionFloat, localMaximumFloat))
                localHit = static_cast<double>(*hit);
        }
    }
    if (!localHit || !finite(*localHit) || *localHit < 0.0)
        return {};
    return *localHit / localScale;
}

bool segmentBlocked(const std::vector<Caster>& casters, const AssetCatalog* assets, DVec3 start, DVec3 target) {
    const DVec3 delta = target - start;
    const double distance = glm::length(delta);
    if (!finite(distance) || distance <= rayEpsilon)
        return false;
    const DVec3 direction = delta / distance;
    for (const auto& caster : casters) {
        const auto hit = intersectCaster(caster, assets, start, direction, distance);
        if (!hit)
            continue;
        // The endpoint is the receiving plane (or its normal-biased point).
        // Ignore endpoint/self hits and tiny origin hits in world units.
        if (*hit > terminalEpsilon && *hit < distance - terminalEpsilon)
            return true;
    }
    return false;
}

double shadowDistance(const std::vector<Caster>& casters, DVec3 target) {
    double maximum = 1.0;
    for (const auto& caster : casters) {
        for (int x = 0; x < 2; ++x)
            for (int y = 0; y < 2; ++y)
                for (int z = 0; z < 2; ++z) {
                    const DVec3 corner{x ? caster.boundsMax.x : caster.boundsMin.x,
                                       y ? caster.boundsMax.y : caster.boundsMin.y,
                                       z ? caster.boundsMax.z : caster.boundsMin.z};
                    const double distance = glm::length(corner - target);
                    if (finite(distance))
                        maximum = std::max(maximum, distance + 1.0);
                }
    }
    return maximum;
}

DVec3 acesAndSrgb(DVec3 radiance, double exposure) {
    DVec3 result{};
    for (int axis = 0; axis < 3; ++axis) {
        // The native tone pass clamps HDR storage to non-negative FP16 values
        // before applying exposure and the fitted ACES curve.
        double c = std::clamp(radiance[axis], 0.0, 65504.0);
        c *= exposure;
        const double mapped = std::clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);
        result[axis] = mapped <= 0.0031308 ? 12.92 * mapped : 1.055 * std::pow(mapped, 1.0 / 2.4) - 0.055;
    }
    return result;
}

DVec3 illuminate(const SceneLight& light, DVec3 ground, DVec3 normal, DVec3 viewDirection, DVec3 albedo,
                 double visibility) {
    DVec3 l{};
    double energy = static_cast<double>(light.intensity);
    if (light.directional) {
        const DVec3 direction = toDouble(light.direction);
        const double length = glm::length(direction);
        if (!finite(length) || length <= cullEpsilon)
            return {};
        l = -direction / length;
    } else {
        const DVec3 delta = toDouble(light.position) - ground;
        const double distance = glm::length(delta);
        const double radius = static_cast<double>(light.radius);
        if (!finite(distance) || !finite(radius) || distance <= cullEpsilon || radius <= 0.0 || distance >= radius)
            return {};
        l = delta / distance;
        const double edge = std::max(1.0 - std::pow(distance / radius, 4.0), 0.0);
        energy *= edge * edge / std::max(distance * distance, 0.01);
    }

    const double normalLight = std::max(glm::dot(normal, l), 0.0);
    if (!finite(normalLight) || normalLight <= 0.0 || !finite(energy) || energy <= 0.0)
        return {};
    const DVec3 halfVectorRaw = viewDirection + l;
    const double halfLength = glm::length(halfVectorRaw);
    if (!finite(halfLength) || halfLength <= cullEpsilon)
        return {};
    const DVec3 halfVector = halfVectorRaw / halfLength;
    const double normalView = std::max(glm::dot(normal, viewDirection), 0.0001);
    const double normalHalf = std::max(glm::dot(normal, halfVector), 0.0);
    const double viewHalf = std::max(glm::dot(viewDirection, halfVector), 0.0);

    constexpr double roughness = 0.65;
    constexpr double metallic = 0.0;
    constexpr double alpha = roughness * roughness;
    const double alphaSquared = alpha * alpha;
    const double denominator = normalHalf * normalHalf * (alphaSquared - 1.0) + 1.0;
    const double distribution = alphaSquared / (pi * denominator * denominator);
    const double visibilityView =
        2.0 * normalView / (normalView + std::sqrt(alphaSquared + (1.0 - alphaSquared) * normalView * normalView));
    const double visibilityLight =
        2.0 * normalLight /
        std::max(normalLight + std::sqrt(alphaSquared + (1.0 - alphaSquared) * normalLight * normalLight), 0.0001);
    const DVec3 f0{0.04};
    const DVec3 fresnel = f0 + (DVec3{1.0} - f0) * std::pow(1.0 - viewHalf, 5.0);
    const DVec3 specular =
        distribution * visibilityView * visibilityLight * fresnel / std::max(4.0 * normalView * normalLight, 0.0001);
    const DVec3 diffuse = (DVec3{1.0} - fresnel) * (1.0 - metallic) * albedo / pi;
    const DVec3 color = toDouble(light.color);
    return (diffuse + specular) * normalLight * energy * color * visibility;
}

double groundVisibility(const std::vector<Caster>& casters, const Scene& scene, const RenderView& view,
                        const SceneLight& light, DVec3 receiver) {
    if (!scene.lighting.shadows || !light.shadows)
        return 1;
    if (light.directional) {
        const double depth = -(toDouble(view.cameraView) * glm::dvec4(receiver, 1)).z;
        if (depth > std::min(view.cameraFar, scene.lighting.shadowDistance))
            return 1;
        const auto direction = glm::normalize(toDouble(light.direction));
        return segmentBlocked(casters, scene.assets.get(), receiver - direction * shadowDistance(casters, receiver),
                              receiver)
                   ? 0
                   : 1;
    }
    return segmentBlocked(casters, scene.assets.get(), toDouble(light.position), receiver) ? 0 : 1;
}
} // namespace

bool stableGroundVisibility(const Scene& scene, const RenderView& view, glm::vec3 groundPosition, float footprint) {
    const auto casters = collectCasters(scene);
    const DVec3 point = toDouble(groundPosition) + DVec3(0, scene.lighting.normalBias, 0);
    for (const auto& light : view.lights) {
        if (light.intensity <= 0 || !light.shadows || !scene.lighting.shadows)
            continue;
        const auto center = groundVisibility(casters, scene, view, light, point);
        for (int x = -1; x <= 1; ++x)
            for (int z = -1; z <= 1; ++z)
                if (groundVisibility(casters, scene, view, light,
                                     point + DVec3(x * double(footprint), 0, z * double(footprint))) != center)
                    return false;
    }
    return true;
}

glm::vec3 expectedGroundRgb(const Scene& scene, const RenderView& view, glm::vec3 groundPosition, glm::vec3 albedo,
                            bool receiveShadows) {
    const DVec3 ground = toDouble(groundPosition);
    const DVec3 baseColor = toDouble(albedo);
    const DVec3 mappedNormalRaw{1.0 / 255.0, 1.0, 1.0 / 255.0};
    const double normalLength = glm::length(mappedNormalRaw);
    const DVec3 normal = normalLength > cullEpsilon ? mappedNormalRaw / normalLength : DVec3{0.0, 1.0, 0.0};
    DVec3 viewDirection = toDouble(view.eye) - ground;
    const double viewLength = glm::length(viewDirection);
    viewDirection = (finite(viewLength) && viewLength > cullEpsilon) ? viewDirection / viewLength : normal;

    const auto casters = collectCasters(scene);
    const DVec3 receiver = ground + DVec3(0, std::max(0.0, static_cast<double>(scene.lighting.normalBias)), 0);

    DVec3 radiance = baseColor * static_cast<double>(scene.lighting.ambient);
    for (const auto& light : view.lights) {
        const double intensity = static_cast<double>(light.intensity);
        if (!finite(intensity) || intensity <= 0.0)
            continue;

        const double visibility = receiveShadows ? groundVisibility(casters, scene, view, light, receiver) : 1;
        radiance += illuminate(light, ground, normal, viewDirection, baseColor, visibility);
    }

    const double exposure = finite(static_cast<double>(view.exposure)) ? static_cast<double>(view.exposure) : 1.0;
    const DVec3 display = acesAndSrgb(radiance, exposure);
    return {static_cast<float>(std::clamp(display.x * 255.0, 0.0, 255.0)),
            static_cast<float>(std::clamp(display.y * 255.0, 0.0, 255.0)),
            static_cast<float>(std::clamp(display.z * 255.0, 0.0, 255.0))};
}

} // namespace proto
