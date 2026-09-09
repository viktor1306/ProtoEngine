#include "editor/Editor.hpp"
#include "renderer/AssetRenderer.hpp"
#include <cmath>

namespace proto {
namespace {
// Independent double-precision reference for the documented M2 studio setup.
// Numerical samples verify the actual shader, descriptor layouts and imported tangent frames.
glm::vec3 studio(glm::dvec3 point, glm::dvec3 normal, glm::dvec3 base, double metallic, double roughness,
                 glm::dvec3 eye) {
    const auto n = glm::normalize(normal), light = glm::normalize(glm::dvec3(.4, .7, 1)),
               view = glm::normalize(eye - point), half = glm::normalize(light + view);
    const auto ndl = std::max(glm::dot(n, light), 0.0), ndv = std::max(glm::dot(n, view), .0001),
               ndh = std::max(glm::dot(n, half), 0.0), vdh = std::max(glm::dot(view, half), 0.0);
    const double alphaSquared = std::pow(roughness, 4), denominator = 1 + (alphaSquared - 1) * ndh * ndh;
    const double distribution = alphaSquared / (3.141592653589793 * denominator * denominator);
    const auto smith = [&](double cosine) {
        return 2 * cosine / std::max(cosine + std::sqrt(alphaSquared + (1 - alphaSquared) * cosine * cosine), .0001);
    };
    const auto f0 = glm::mix(glm::dvec3(.04), base, metallic), fresnel = f0 + (1.0 - f0) * std::pow(1 - vdh, 5);
    auto radiance = ((1.0 - fresnel) * (1 - metallic) * base / 3.141592653589793 +
                     distribution * smith(ndv) * smith(ndl) * fresnel / std::max(4 * ndv * ndl, .0001)) *
                        ndl * 3.0 +
                    base * .035;
    glm::vec3 result;
    for (int i = 0; i < 3; ++i) {
        const double v = radiance[i];
        const double mapped = std::clamp((v * (2.51 * v + .03)) / (v * (2.43 * v + .59) + .14), 0.0, 1.0);
        result[i] = float(255 * (mapped <= .0031308 ? 12.92 * mapped : 1.055 * std::pow(mapped, 1 / 2.4) - .055));
    }
    return result;
}
} // namespace
void Editor::startAssetSmoke(const std::filesystem::path& source, const std::filesystem::path& scene) {
    assetSmoke_ = true;
    document_.scene = Scene{};
    document_.scene.name = "M2 Material Lab";
    document_.selection = {};
    document_.save(scene);
    importScene_ = document_.scene.id;
    importInstance_ = true;
    importJob_.start(document_.workspace(), source);
    showGrid_ = false;
}
void Editor::assetSmokeStep(uint64_t frame) {
    if (!assetSmoke_)
        return;
    if (showError_)
        throw std::runtime_error(error_);
    if (!renderer_.assetRenderer()->error().empty())
        throw std::runtime_error(renderer_.assetRenderer()->error());
    if (importJob_.busy())
        return;
    if (assetSmokeStage_ == 0 && !document_.scene.assets->models.empty()) {
        const auto model = document_.scene.assets->models.begin()->second;
        if (model->nodes.size() != 10 || model->meshes.size() != 6)
            throw std::runtime_error("Asset smoke requires the deterministic M2 probe fixture");
        const auto before = document_.scene.assets->meshes.at(model->meshes[0]->id);
        const auto second = document_.instantiate(model);
        auto copy = document_.scene.record(document_.scene.find(second));
        copy.transform.position.z = -.2f;
        document_.edit(copy);
        if (document_.scene.assets->meshes.at(before->id) != before)
            throw std::runtime_error("Duplicate instance copied geometry");
        camera_.pivot = {0, -1, 0};
        camera_.yaw = camera_.pitch = 0;
        camera_.distance = 10.5f;
        assetSmokeFrame_ = frame;
        assetSmokeStage_ = 1;
    }
    if (assetSmokeStage_ == 1 && frame > assetSmokeFrame_ + 12 && renderer_.assetRenderer()->pending() == 0) {
        const auto model = document_.scene.assets->models.begin()->second;
        const auto id = model->materials[0]->id;
        const auto original = document_.scene.assets->materials.at(id)->values;
        const auto history = document_.undoCount();
        document_.beginMaterialGesture(id);
        for (int i = 0; i < 20; ++i) {
            auto v = original;
            v.roughness = .2f + .01f * float(i);
            document_.previewMaterial(id, v);
        }
        document_.endGesture();
        if (document_.undoCount() != history + 1)
            throw std::runtime_error("Material gesture history mismatch");
        document_.undo();
        document_.redo();
        document_.undo();
        if (document_.scene.assets->materials.at(id)->values != original)
            throw std::runtime_error("Material Undo/Redo mismatch");
        document_.save(document_.path());
        const auto bytes = encodeScene(document_.scene);
        document_.load(document_.path());
        if (encodeScene(document_.scene) != bytes)
            throw std::runtime_error("M2 save/reopen changed scene");
        for (const auto& [sourceId, bundle] : document_.scene.assets->models)
            if (!bundle->fromCache || bundle->decodedImages)
                throw std::runtime_error("Reopen decoded source images");
        for (const auto handle : document_.scene.meshes())
            if (document_.scene.entity(handle).name == "Normal map") {
                document_.selection = document_.scene.entity(handle).id;
                break;
            }
        assetSmokeFrame_ = frame;
        assetSmokeStage_ = 2;
    }
    if (assetSmokeStage_ == 2 && frame > assetSmokeFrame_ + 12 && renderer_.assetRenderer()->pending() == 0) {
        if (renderer_.assetRenderer()->meshCount() != 6 || renderer_.assetRenderer()->imageCount() != 7 ||
            renderer_.assetRenderer()->uploadedBytes() != 0)
            throw std::runtime_error("GPU asset sharing or idle upload check failed");
        assetSmokePassed_ = true;
    }
}
void Editor::assetColorProbes() {
    const auto project = [&](glm::vec3 point) {
        const auto p = sceneView_.viewProjection * glm::vec4(point, 1);
        return glm::vec3(p.x / p.w * .5f + .5f, .5f - p.y / p.w * .5f, p.z / p.w);
    };
    const auto color = [&](const char* label, glm::vec3 point, glm::vec3 rgb, float tolerance = 4) {
        const auto p = project(point);
        sceneView_.colorProbes.push_back({p.x, p.y, rgb, tolerance, label});
        sceneView_.depthProbes.push_back({p.x, p.y, p.z});
    };
    const glm::dvec3 normal(204.0 / 127.5 - 1, -(128.0 / 127.5 - 1), 230.0 / 127.5 - 1);
    const auto shaded = [&](const char* label, glm::vec3 point, glm::dvec3 n, glm::dvec3 base = glm::dvec3(.45),
                            double metallic = 0, double roughness = .8) {
        color(label, point, studio(point, n, base, metallic, roughness, sceneView_.eye), 5);
    };
    shaded("normal map", {-2.4f, 1.4f, 0}, normal);
    shaded("mirrored double-sided normal", {0, 1.4f, 0}, {-normal.x, normal.y, normal.z});
    shaded("flat reference", {2.4f, 1.4f, 0}, {1.0 / 255, -1.0 / 255, 1});
    shaded("double-sided back face", {-2.4f, -3.4f, 0}, {normal.x, -normal.y, normal.z});
    shaded("packed MR channels", {.45f, -3.4f, 0}, {1.0 / 255, -1.0 / 255, 1}, {1, .55, .1}, 200.0 / 255,
           .4 * 153 / 255);
    color("MASK solid", {-2.0f, -1, 0}, {80, 200, 120});
    color("mirrored MASK solid", {-.4f, -1, 0}, {80, 200, 120});
    color("JPEG and UV transform", {2.0f, -.6f, 0}, {0, 255, 0}, 12);
    color("OPAQUE ignores texture alpha", {2.0f, -3.4f, 0}, {80, 200, 120});
    for (const glm::vec3 point : {glm::vec3(-2.8f, -1, 0), glm::vec3(.4f, -1, 0)}) {
        const auto p = project(point);
        sceneView_.depthProbes.push_back({p.x, p.y, 1});
    }
}
} // namespace proto
