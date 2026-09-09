#include "runtime/SceneViewBuilder.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace proto {
namespace {
bool inFrustum(const Bounds& bounds, const std::array<glm::vec4, 6>& planes) {
    const glm::vec3 center = (bounds.min + bounds.max) * .5f;
    const glm::vec3 half = (bounds.max - bounds.min) * .5f;
    for (const auto& plane : planes)
        if (glm::dot(glm::vec3(plane), center) + plane.w + glm::dot(glm::abs(glm::vec3(plane)), half) < 0)
            return false;
    return true;
}
} // namespace

void SceneViewBuilder::buildRuntimeView(Scene& scene, VkExtent2D extent, RenderView& view) {
    scene.update();

    const float width = static_cast<float>(std::max(extent.width, 1u));
    const float height = static_cast<float>(std::max(extent.height, 1u));
    const float aspect = width / height;
    Camera camera;
    glm::mat4 cameraView;
    const glm::vec3 fallbackEye(7, 5, 9);
    glm::vec3 eye = fallbackEye;
    bool hasCamera = false;

    const auto activeCamera = scene.find(scene.activeCamera);
    if (activeCamera && scene.camera(activeCamera)) {
        const auto& transform = scene.transform(activeCamera);
        if (transform.visible && !transform.degenerate) {
            const auto world = transform.world;
            eye = glm::vec3(world[3]);
            const auto direction = glm::normalize(glm::vec3(world[2]));
            const auto up = glm::normalize(glm::vec3(world[1]));
            if (glm::length(direction) > 0 && glm::length(up) > 0) {
                cameraView = glm::lookAtRH(eye, eye - direction, up);
                camera = *scene.camera(activeCamera);
                hasCamera = true;
            }
        }
    }
    if (!hasCamera) {
        camera = Camera{};
        eye = fallbackEye;
        cameraView = glm::lookAtRH(eye, glm::vec3(0), glm::vec3(0, 1, 0));
    }

    view.viewProjection =
        glm::perspectiveRH_ZO(glm::radians(camera.verticalFovDegrees), aspect, camera.nearPlane, camera.farPlane) *
        cameraView;
    view.cameraView = cameraView;
    view.eye = eye;
    view.verticalFovDegrees = camera.verticalFovDegrees;
    view.cameraNear = camera.nearPlane;
    view.cameraFar = camera.farPlane;
    view.exposure = camera.exposure;

    view.items.clear();
    view.imported.clear();
    view.depthProbes.clear();
    view.colorProbes.clear();
    view.lights.clear();
    view.casters.clear();
    view.grid = false;
    view.assets = scene.assets;
    view.exactDepth = false;
    view.lightingMode = LightingMode::Scene;
    view.lighting = scene.lighting;
    view.runtimeLighting = {};

    for (const auto handle : scene.directionalLights()) {
        const auto& transform = scene.transform(handle);
        if (!transform.visible || transform.degenerate)
            continue;
        const auto& light = *scene.directionalLight(handle);
        view.lights.push_back({scene.entity(handle).id, glm::vec3(transform.world[3]),
                               -glm::normalize(glm::vec3(transform.world[2])), light.color, light.intensity, 8, true,
                               light.shadows});
    }
    for (const auto handle : scene.pointLights()) {
        const auto& transform = scene.transform(handle);
        if (!transform.visible || transform.degenerate)
            continue;
        const auto& light = *scene.pointLight(handle);
        view.lights.push_back({scene.entity(handle).id, glm::vec3(transform.world[3]), glm::vec3(0, -1, 0), light.color,
                               light.intensity, light.radius, false, light.shadows});
    }

    const auto rows = glm::transpose(view.viewProjection);
    const std::array<glm::vec4, 6> planes{rows[3] + rows[0], rows[3] - rows[0], rows[3] + rows[1],
                                         rows[3] - rows[1], rows[2], rows[3] - rows[2]};
    for (const auto handle : scene.meshes()) {
        const auto& transform = scene.transform(handle);
        const auto& mesh = *scene.mesh(handle);
        if (!transform.visible || transform.degenerate)
            continue;
        ImportedItem item{mesh.mesh,      transform.world,         transform.normal, mesh.color,
                          mesh.materials, scene.entity(handle).id, transform.bounds, mesh.receiveShadows};
        if (mesh.castShadows)
            view.casters.push_back(item);
        if (!inFrustum(transform.bounds, planes))
            continue;
        // Runtime views always use the scene lighting path. Built-in meshes
        // therefore use the same PBR/shadow path as imported meshes.
        view.imported.push_back(item);
    }
}
} // namespace proto
