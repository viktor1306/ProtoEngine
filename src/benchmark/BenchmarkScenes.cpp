#include "benchmark/BenchmarkScenes.hpp"
#include "assets/AssetIO.hpp"
#include "assets/AssetWorkspace.hpp"
#include "assets/PortableAssets.hpp"
#include "core/Diagnostics.hpp"
#include "scene/SceneIO.hpp"
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace proto {
namespace {
template<class Id> Id fixedId(uint64_t value) {
    Id result;
    result.uuid.bytes[0] = 0x77;
    result.uuid.bytes[6] = 0x40;
    result.uuid.bytes[8] = 0x80;
    for (size_t i = 0; i < 7; ++i)
        result.uuid.bytes[15 - i] = uint8_t(value >> (i * 8));
    return result;
}
EntityId entityId(uint64_t value) { return fixedId<EntityId>(value); }
AssetId assetId(uint64_t value) { return fixedId<AssetId>(0x10000000u + value); }

std::shared_ptr<ModelBundle> makeMaskModel() {
    auto model = std::make_shared<ModelBundle>();
    model->id = assetId(1);
    model->name = "M7 original textured MASK";
    model->revision = "m7-fixture-v1";
    auto pixels = std::make_shared<TexturePixels>();
    pixels->srgb = true;
    std::vector<uint8_t> rgba(64 * 64 * 4);
    for (size_t y = 0; y < 64; ++y)
        for (size_t x = 0; x < 64; ++x) {
            const auto at = (y * 64 + x) * 4;
            rgba[at] = uint8_t(70 + x);
            rgba[at + 1] = 190;
            rgba[at + 2] = 125;
            rgba[at + 3] = ((x / 8 + y / 8) % 2) ? 255 : 0;
        }
    pixels->mips = makeMips(64, 64, rgba, true, false, .5f);
    pixels->hash = "m7-mask:" + sha256(rgba);
    auto texture = std::make_shared<TextureAsset>();
    texture->id = assetId(2);
    texture->pixels = pixels;
    auto material = std::make_shared<MaterialAsset>();
    material->id = assetId(3);
    material->owner = model->id;
    material->name = "PBR cutout";
    material->values.mask = true;
    material->values.doubleSided = true;
    material->values.metallic = .1f;
    material->values.roughness = .5f;
    material->textures[0].texture = texture->id;
    auto mesh = std::make_shared<MeshAsset>();
    mesh->id = assetId(4);
    mesh->name = "Shared quad";
    mesh->revision = "m7-quad-v1";
    mesh->vertices = {{{-1,-1,0},{0,0,1},{1,0,0,1},{1,1,1,1},{0,0},{0,0}},
                      {{1,-1,0},{0,0,1},{1,0,0,1},{1,1,1,1},{1,0},{1,0}},
                      {{1,1,0},{0,0,1},{1,0,0,1},{1,1,1,1},{1,1},{1,1}},
                      {{-1,1,0},{0,0,1},{1,0,0,1},{1,1,1,1},{0,1},{0,1}}};
    mesh->indices = {0,1,2,0,2,3};
    mesh->parts = {{0,6,material->id}};
    mesh->bounds = {{-1,-1,0},{1,1,0}};
    buildMeshBvh(*mesh);
    model->textures = {texture};
    model->materials = {material};
    model->meshes = {mesh};
    model->nodes = {{"MASK",-1,glm::mat4(1),mesh->id}};
    return model;
}
void remember(BenchmarkScene& result, const EntityRecord& entity) {
    result.moving.push_back(entity.id);
    result.originalTransforms.push_back(entity.transform);
}
}

std::vector<std::string> benchmarkScenarioNames() {
    return {"empty", "small", "instances-1000", "instances-10000-visible", "instances-10000-culled",
            "instances-10000-moving", "lights-1", "lights-8", "lights-32", "lights-128",
            "moving-lights-32", "moving-casters", "pool-overflow", "pool-resident"};
}
BenchmarkScene makeBenchmarkScene(std::string_view name) {
    const auto names = benchmarkScenarioNames();
    if (std::find(names.begin(), names.end(), name) == names.end())
        throw std::runtime_error("Unknown benchmark scene: " + std::string(name));
    BenchmarkScene result;
    result.scenario = name;
    result.scene.id = assetId(100);
    result.scene.name = "M7 " + std::string(name);
    if (name == "pool-overflow" || name == "pool-resident")
        result.scene.name = "M7 pool comparison";
    result.graphics = playerGraphicsProfile("Balanced");
    result.graphics.profile = "Custom";
    result.graphics.vSync = false;
    result.graphics.sunShadows = {true,3,1024,60,"pcf3x3"};
    result.graphics.pointShadows = {true,256,9,48,"multiPass"};
    result.graphics.viewDistance = 500;

    const bool instances = name.starts_with("instances-");
    EntityRecord camera;
    camera.id = entityId(1);
    camera.name = "Fixed benchmark camera";
    camera.camera = Camera{};
    camera.transform.position = instances ? glm::vec3(0,95,145) : glm::vec3(11,8,14);
    camera.transform.rotation = glm::quatLookAt(glm::normalize(glm::vec3(0,.5f,0) - camera.transform.position),
                                               glm::vec3(0,1,0));
    result.scene.create(camera);
    result.scene.activeCamera = camera.id;
    if (name != "empty") {
        EntityRecord floor;
        floor.id = entityId(2);
        floor.name = "Receiver";
        floor.mesh = MeshRenderer{builtin::plane, {.28f,.31f,.35f,1}};
        floor.transform.scale = instances ? glm::vec3(125,1,125) : glm::vec3(25,1,25);
        result.scene.create(floor);
        ++result.expectedMeshes;
        EntityRecord parent;
        parent.id = entityId(3);
        parent.name = "Shared parent";
        result.scene.create(parent);
        size_t count = instances ? (name == "instances-1000" ? 1000u : 10000u) : 36u;
        const auto side = static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(count))));
        for (size_t i = 0; i < count; ++i) {
            EntityRecord box;
            box.id = entityId(1000 + i);
            box.parent = parent.id;
            box.name = "Instance " + std::to_string(i);
            box.mesh = MeshRenderer{builtin::cube, {.2f,.65f,.6f,1}};
            const float spacing = instances ? 1.15f : 1.7f;
            box.transform.position = {(float(i % side) - float(side - 1) * .5f) * spacing,
                                      .5f, (float(i / side) - float(side - 1) * .5f) * spacing};
            if (name == "instances-10000-culled" && i >= 100)
                box.transform.position.x += 10000;
            result.scene.create(box);
            if (name == "moving-casters" && i < 12)
                remember(result, box);
        }
        result.expectedMeshes += count;
        if (name == "instances-10000-moving") {
            result.motion = BenchmarkScene::Motion::Parent;
            remember(result, parent);
        } else if (name == "moving-casters")
            result.motion = BenchmarkScene::Motion::Casters;
        if (instances)
            result.graphics.sunShadows.distance = 350;

        auto mask = makeMaskModel();
        result.scene.assets->publish(mask);
        result.scene.modelSources = {mask->id};
        for (uint64_t i = 0; i < 3; ++i) {
            EntityRecord cutout;
            cutout.id = entityId(10 + i);
            cutout.parent = parent.id;
            cutout.name = "Shared MASK " + std::to_string(i);
            cutout.mesh = MeshRenderer{mask->meshes[0]->id, glm::vec4(1)};
            cutout.transform.position = {-3 + float(i) * 3, 2.0f, -7};
            result.scene.create(cutout);
            ++result.expectedMeshes;
        }
        EntityRecord sun;
        sun.id = entityId(4);
        sun.name = "Sun";
        sun.directionalLight = DirectionalLight{};
        sun.directionalLight->intensity = 1.5f;
        sun.transform.rotation = glm::quat(glm::radians(glm::vec3(-50,-25,0)));
        result.scene.create(sun);

        size_t lights = instances ? 1 : 4;
        if (name.starts_with("lights-"))
            lights = static_cast<size_t>(std::stoul(std::string(name.substr(7))));
        if (name == "moving-lights-32") {
            lights = 32;
            result.motion = BenchmarkScene::Motion::Lights;
        }
        if (name == "pool-overflow" || name == "pool-resident") {
            lights = 8;
            result.graphics.pointShadows.poolBudgetMiB = name == "pool-overflow" ? 2 : 16;
        }
        result.expectedPointLights = lights;
        for (size_t i = 0; i < lights; ++i) {
            EntityRecord light;
            light.id = entityId(100 + i);
            light.name = "Point " + std::to_string(i);
            light.pointLight = PointLight{};
            const auto angle = float(i) * 2 * std::numbers::pi_v<float> / float(lights);
            light.transform.position = {std::cos(angle) * 4, 4 + .1f * float(i % 3), std::sin(angle) * 4};
            light.pointLight->radius = instances ? 25.f : 18.f;
            light.pointLight->intensity = 90.f / float(lights);
            result.scene.create(light);
            if (result.motion == BenchmarkScene::Motion::Lights)
                remember(result, light);
        }
    }
    result.scene.update();
    std::string hashInput = encodeScene(result.scene);
    std::vector<AssetId> modelIds = result.scene.modelSources;
    std::sort(modelIds.begin(), modelIds.end());
    for (const auto id : modelIds)
        hashInput += sha256(encodePortableModel(*result.scene.assets->models.at(id)));
    // Profile is recorded separately so pool-overflow/resident can compare
    // identical content with intentionally different memory budgets.
    result.contentHash = sha256(hashInput);
    return result;
}
void BenchmarkScene::advance(uint64_t frame) {
    advanceAt(double(frame % 240) / 60.0);
}
void BenchmarkScene::advanceAt(double seconds) {
    const auto phase = 2.f * std::numbers::pi_v<float> * float(std::fmod(seconds, 4.0)) / 4.f;
    for (size_t i = 0; i < moving.size(); ++i) {
        auto transform = originalTransforms[i];
        if (motion == Motion::Parent)
            transform.rotation = glm::angleAxis(.15f * std::sin(phase), glm::vec3(0,1,0));
        else if (motion == Motion::Lights) {
            transform.position.x += std::sin(phase + float(i)) * 1.5f;
            transform.position.z += std::cos(phase + float(i)) * 1.5f;
        } else if (motion == Motion::Casters) {
            transform.position.y += .35f * (1 + std::sin(phase + float(i)));
            transform.rotation = glm::angleAxis(phase, glm::vec3(0,1,0));
        }
        scene.setTransformDeferred(scene.find(moving[i]), transform);
    }
}
std::string benchmarkSceneManifest(const BenchmarkScene& scene) {
    uint64_t triangles{};
    for (const auto handle : scene.scene.meshes()) {
        const auto id = scene.scene.mesh(handle)->mesh;
        if (id == builtin::cube) triangles += 12;
        else if (id == builtin::plane) triangles += 2;
        else triangles += scene.scene.assets->meshes.at(id)->indices.size() / 3;
    }
    return "{\"format\":\"proto.benchmark.scene\",\"version\":1,\"scenario\":" + jsonString(scene.scenario) +
           ",\"contentHash\":" + jsonString(scene.contentHash) + ",\"entities\":" +
           std::to_string(scene.scene.entities().size()) + ",\"meshes\":" +
           std::to_string(scene.expectedMeshes) + ",\"authoredTriangles\":" + std::to_string(triangles) +
           ",\"geometry\":\"shared 12-triangle cubes, plane and original PBR MASK quads\",\"pointLights\":" + std::to_string(scene.expectedPointLights) +
           ",\"directionalLights\":" + std::to_string(scene.scene.directionalLights().size()) +
           ",\"movingTransforms\":" + std::to_string(scene.moving.size()) +
           ",\"animation\":\"continuous four-second cycle; timed origin zero; capture frame at 60 Hz\",\"graphics\":" + encodePlayerGraphics(scene.graphics) + "}";
}
}
